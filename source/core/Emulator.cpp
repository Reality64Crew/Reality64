#include "core/Emulator.h"

#include <algorithm>
#include <cstring>

namespace reality64 {

namespace {

constexpr size_t Ipl3CopySize = 0x100000;  // the boot code copies the first 1 MiB after the header block

uint64_t sx32(uint32_t v) {
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v)));
}

void writeBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

// The last character of the game code is the destination region.
enum class Region { Ntsc, Pal, Mpal };

Region regionFromGameCode(const std::string& code) {
    if (code.size() < 4) return Region::Ntsc;
    switch (code[3]) {
        case 'D': case 'F': case 'H': case 'I': case 'L': case 'P':
        case 'S': case 'U': case 'W': case 'X': case 'Y':
            return Region::Pal;
        case 'B':
            return Region::Mpal;
        default:
            return Region::Ntsc;
    }
}

}  // namespace

bool Emulator::loadRom(const std::string& path, std::string& error) {
    auto rom = std::make_shared<Rom>();
    if (!rom->loadFromFile(path, error)) return false;
    rom_ = std::move(rom);
    bus_.attachRom(rom_);
    return true;
}

// High-level approximation of the PIF + IPL3 boot sequence.
bool Emulator::boot(std::string& error) {
    if (!rom_) {
        error = "no ROM loaded";
        return false;
    }
    const std::vector<uint8_t>& data = rom_->data();

    cic_ = detectCic(data);
    const uint32_t entry = rom_->header().bootAddress - cicEntryAdjust(cic_);
    if (entry < 0x80000000u || entry >= 0xC0000000u) {
        error = "boot address is not in KSEG0/KSEG1";
        return false;
    }

    const uint32_t entryPhys = entry & 0x1FFFFFFFu;
    const size_t copySize = std::min(data.size() - Rom::BootCodeSize, Ipl3CopySize);
    if (entryPhys + copySize > MemoryBus::RdramSize) {
        error = "boot code does not fit in RDRAM";
        return false;
    }

    const Region region = regionFromGameCode(rom_->header().gameCode);
    pal_ = region == Region::Pal;

    bus_.resetDevices();
    bus_.applyBootRegisterState();
    bus_.setVideoStandard(pal_, forcedRefreshHz_);
    std::memset(bus_.rdram(), 0, MemoryBus::RdramSize);

    // The PIF puts the first 4 KiB of the cart into SP DMEM...
    std::memcpy(bus_.spMem(), data.data(), Rom::BootCodeSize);
    // ...and IPL3 then copies the game's code to its link address.
    std::memcpy(bus_.rdram() + entryPhys, data.data() + Rom::BootCodeSize, copySize);

    // OS globals libultra reads at startup.
    uint8_t* rdram = bus_.rdram();
    writeBe32(rdram + 0x300, region == Region::Pal ? 0 : (region == Region::Mpal ? 2 : 1));  // osTvType
    writeBe32(rdram + 0x308, 0xB0000000);                                                    // osRomBase
    writeBe32(rdram + 0x318, MemoryBus::RdramSize);                                          // osMemSize

    cpu_.reset();
    cpu_.setGpr(11, sx32(0xA4000040));               // t3
    cpu_.setGpr(20, region == Region::Pal ? 0 : 1);  // s4: TV type
    cpu_.setGpr(22, cicSeed(cic_));                  // s6: CIC seed
    cpu_.setGpr(29, sx32(0xA4001FF0));               // sp
    cpu_.setGpr(31, sx32(0xA4001550));               // ra
    cpu_.setPc(sx32(entry));
    return true;
}

uint64_t Emulator::execute(uint64_t maxSteps, bool untilFrame) {
    uint64_t executed = 0;
    const uint64_t startFrame = bus_.frameCount();
    while (executed < maxSteps && !cpu_.halted()) {
        const uint64_t cpi = cpu_.cyclesPerInstruction();
        // The bus reports time in cycles; run that many instructions' worth.
        const uint64_t slice = std::min(maxSteps - executed, std::max<uint64_t>(1, bus_.cyclesUntilNextEvent() / cpi));
        const uint64_t done = cpu_.run(slice);
        executed += done;
        bus_.advance(done * cpi);
        if (untilFrame && bus_.frameCount() != startFrame) break;
    }
    return executed;
}

uint64_t Emulator::run(uint64_t maxSteps) { return execute(maxSteps, false); }

uint64_t Emulator::runFrame() {
    // Two frames' worth of cycles is a safety net if the VI never wraps.
    return execute(2 * (MemoryBus::CpuClockHz / 50) / cpu_.cyclesPerInstruction(), true);
}

namespace {

constexpr uint32_t StateMagic = 0x53343652;     // "R64S"
constexpr uint32_t StateEndMagic = 0x45343652;  // "R64E"
constexpr uint32_t StateVersion = 1;

}  // namespace

std::vector<uint8_t> Emulator::saveState() const {
    StateWriter w;
    w.u32(StateMagic);
    w.u32(StateVersion);
    if (rom_) {
        w.u32(rom_->header().crc1);
        w.u32(rom_->header().crc2);
        w.string(rom_->header().name);
    } else {
        w.u32(0);
        w.u32(0);
        w.string("");
    }
    w.u32(static_cast<uint32_t>(cic_));
    w.boolean(pal_);
    cpu_.saveState(w);
    bus_.saveState(w);
    w.u32(StateEndMagic);
    return w.take();
}

bool Emulator::loadState(const std::vector<uint8_t>& data, std::string& error) {
    StateReader r(data.data(), data.size());
    if (r.u32() != StateMagic) {
        error = "this is not a Reality64 save state";
        return false;
    }
    if (r.u32() != StateVersion) {
        error = "this save state was made by an incompatible version";
        return false;
    }
    const uint32_t crc1 = r.u32();
    const uint32_t crc2 = r.u32();
    const std::string name = r.string();
    if (!rom_ || crc1 != rom_->header().crc1 || crc2 != rom_->header().crc2 || name != rom_->header().name) {
        error = "this save state belongs to a different game";
        return false;
    }

    // The footer is checked before anything is applied, so a truncated file
    // cannot leave the machine half-restored.
    if (data.size() < 4 || StateReader(data.data() + data.size() - 4, 4).u32() != StateEndMagic) {
        error = "the save state file is truncated or damaged";
        return false;
    }

    const uint32_t cic = r.u32();
    const bool pal = r.boolean();
    cpu_.loadState(r);
    bus_.loadState(r);
    if (!r.ok() || r.u32() != StateEndMagic) {
        error = "the save state file is damaged";
        return false;
    }
    cic_ = static_cast<Cic>(cic);
    pal_ = pal;
    return true;
}

}  // namespace reality64
