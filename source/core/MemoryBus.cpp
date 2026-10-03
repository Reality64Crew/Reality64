#include "core/MemoryBus.h"

#include <algorithm>
#include <cstring>

namespace reality64 {

namespace {

constexpr uint32_t MmioStart = 0x04040000;
constexpr uint32_t MmioEnd = 0x04900000;

constexpr uint32_t SpRegs = 0x04040000;
constexpr uint32_t SpPcReg = 0x04080000;
constexpr uint32_t DpcRegs = 0x04100000;
constexpr uint32_t DpsRegs = 0x04200000;
constexpr uint32_t MiRegs = 0x04300000;
constexpr uint32_t ViRegsBase = 0x04400000;
constexpr uint32_t AiRegs = 0x04500000;
constexpr uint32_t PiRegs = 0x04600000;
constexpr uint32_t RiRegs = 0x04700000;
constexpr uint32_t SiRegs = 0x04800000;

constexpr uint32_t SpStatusHalt = 1u << 0;
constexpr uint32_t SpStatusBroke = 1u << 1;
constexpr uint32_t SpStatusSingleStep = 1u << 5;
constexpr uint32_t SpStatusIntrBreak = 1u << 6;
constexpr uint32_t SpStatusSignal0 = 1u << 7;

constexpr uint32_t AiFullStatus = 0x80000001u;
constexpr uint32_t AiBusyStatus = 0x40000000u;
constexpr uint32_t AiDacClock = 48681812;

constexpr uint64_t PiBaseDelay = 200;
constexpr uint64_t SiDelay = 2000;

bool inRange(uint32_t a, uint32_t base, uint32_t bytes) { return a >= base && a - base < bytes; }

}  // namespace

MemoryBus::MemoryBus() : rdram_(RdramSize, 0), spMem_(SpMemSize, 0) { resetDevices(); }

void MemoryBus::resetDevices() {
    spMemAddr_ = spDramAddr_ = spSemaphore_ = spPc_ = 0;
    spStatus_ = SpStatusHalt;
    dpc_.fill(0);
    dps_.fill(0);
    miMode_ = miIntr_ = miMask_ = 0;
    vi_.fill(0);
    aiDramAddr_ = aiControl_ = aiDacRate_ = aiBitRate_ = 0;
    aiCount_ = 0;
    aiEndCycle_ = 0;
    piDramAddr_ = piCartAddr_ = 0;
    piDomain_.fill(0);
    piBusy_ = false;
    piDoneCycle_ = 0;
    ri_.fill(0);
    siDramAddr_ = 0;
    siBusy_ = false;
    siDoneCycle_ = 0;
    pifRam_.fill(0);
    cycles_ = 0;
    viLine_ = 0;
    viLineProgress_ = 0;
    frameCount_ = 0;
}

void MemoryBus::applyBootRegisterState() {
    ri_[0] = 0x0E;        // RI_MODE
    ri_[1] = 0x40;        // RI_CONFIG
    ri_[3] = 0x14;        // RI_SELECT: RDRAM initialised
    ri_[4] = 0x00063634;  // RI_REFRESH
}

void MemoryBus::setVideoStandard(bool pal) {
    frameCycles_ = CpuClockHz / (pal ? 50 : 60);
    defaultLines_ = pal ? 625 : 525;
}

// ---------------------------------------------------------------------------
// Memory map
// ---------------------------------------------------------------------------

const uint8_t* MemoryBus::mapRead(uint32_t paddr, unsigned size) const {
    if (paddr < RdramSize) {
        return paddr + size <= RdramSize ? &rdram_[paddr] : nullptr;
    }
    if (paddr >= SpMemBase && paddr < SpMemBase + SpMemSize) {
        const uint32_t off = paddr - SpMemBase;
        return off + size <= SpMemSize ? &spMem_[off] : nullptr;
    }
    if (rom_ && paddr >= CartBase && paddr < CartEnd) {
        const uint64_t off = paddr - CartBase;
        const auto& data = rom_->data();
        return off + size <= data.size() ? &data[static_cast<size_t>(off)] : nullptr;
    }
    if (paddr >= PifRamBase && paddr < PifRamBase + PifRamSize) {
        const uint32_t off = paddr - PifRamBase;
        return off + size <= PifRamSize ? &pifRam_[off] : nullptr;
    }
    return nullptr;
}

uint8_t* MemoryBus::mapWrite(uint32_t paddr, unsigned size) {
    if (paddr < RdramSize) {
        return paddr + size <= RdramSize ? &rdram_[paddr] : nullptr;
    }
    if (paddr >= SpMemBase && paddr < SpMemBase + SpMemSize) {
        const uint32_t off = paddr - SpMemBase;
        return off + size <= SpMemSize ? &spMem_[off] : nullptr;
    }
    if (paddr >= PifRamBase && paddr < PifRamBase + PifRamSize) {
        const uint32_t off = paddr - PifRamBase;
        return off + size <= PifRamSize ? &pifRam_[off] : nullptr;
    }
    return nullptr;  // cartridge ROM is read-only
}

uint64_t MemoryBus::readSlow(uint32_t paddr, unsigned size) {
    if (paddr >= MmioStart && paddr < MmioEnd) return readMmio(paddr, size);

    const uint8_t* p = mapRead(paddr, size);
    if (!p) {
        ++unmappedAccesses_;
        return 0;
    }
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) {
        value = (value << 8) | p[i];
    }
    return value;
}

void MemoryBus::writeSlow(uint32_t paddr, unsigned size, uint64_t value) {
    if (paddr >= MmioStart && paddr < MmioEnd) {
        writeMmio(paddr, size, value);
        return;
    }
    uint8_t* p = mapWrite(paddr, size);
    if (!p) {
        ++unmappedAccesses_;
        return;
    }
    for (unsigned i = size; i-- > 0;) {
        p[i] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}

// Hardware registers are 32 bits wide; narrower and wider accesses are
// synthesised from word accesses.
uint64_t MemoryBus::readMmio(uint32_t paddr, unsigned size) {
    const uint32_t base = paddr & ~3u;
    if (size == 8) {
        return (static_cast<uint64_t>(readRegister(base)) << 32) | readRegister(base + 4);
    }
    const uint32_t word = readRegister(base);
    if (size == 4) return word;
    const unsigned shift = (4 - size - (paddr & 3)) * 8;
    return (word >> shift) & ((1u << (size * 8)) - 1);
}

void MemoryBus::writeMmio(uint32_t paddr, unsigned size, uint64_t value) {
    const uint32_t base = paddr & ~3u;
    if (size == 8) {
        writeRegister(base, static_cast<uint32_t>(value >> 32));
        writeRegister(base + 4, static_cast<uint32_t>(value));
        return;
    }
    if (size == 4) {
        writeRegister(base, static_cast<uint32_t>(value));
        return;
    }
    const unsigned shift = (4 - size - (paddr & 3)) * 8;
    writeRegister(base, static_cast<uint32_t>(value << shift));
}

uint32_t MemoryBus::readRegister(uint32_t a) {
    if (inRange(a, SpRegs, 0x20)) {
        switch (a - SpRegs) {
            case 0x00: return spMemAddr_;
            case 0x04: return spDramAddr_;
            case 0x08:
            case 0x0C: return 0;
            case 0x10: return spStatus_;
            case 0x14:
            case 0x18: return 0;
            case 0x1C: {
                const uint32_t v = spSemaphore_;
                spSemaphore_ = 1;  // reading acquires the semaphore
                return v;
            }
        }
    } else if (a == SpPcReg) {
        return spPc_ & 0xFFC;
    } else if (a == SpPcReg + 4) {
        return 0;
    } else if (inRange(a, DpcRegs, 0x20)) {
        return dpc_[(a - DpcRegs) >> 2];
    } else if (inRange(a, DpsRegs, 0x10)) {
        return dps_[(a - DpsRegs) >> 2];
    } else if (inRange(a, MiRegs, 0x10)) {
        switch (a - MiRegs) {
            case 0x00: return miMode_;
            case 0x04: return 0x02020102;  // MI_VERSION
            case 0x08: return miIntr_;
            case 0x0C: return miMask_;
        }
    } else if (inRange(a, ViRegsBase, 0x38)) {
        const unsigned index = (a - ViRegsBase) >> 2;
        if (index < vi_.size()) return index == 4 ? viCurrentLine() : vi_[index];
    } else if (inRange(a, AiRegs, 0x18)) {
        switch (a - AiRegs) {
            case 0x00: return aiDramAddr_;
            case 0x04: {  // bytes left in the buffer being played
                if (aiCount_ == 0) return 0;
                const uint64_t total = aiBufferCycles(aiQueue_[0].length);
                const uint64_t left = aiEndCycle_ > cycles_ ? aiEndCycle_ - cycles_ : 0;
                return total ? static_cast<uint32_t>((static_cast<uint64_t>(aiQueue_[0].length) * left / total) & ~7ull) : 0;
            }
            case 0x08: return aiControl_;
            case 0x0C: return (aiCount_ >= 2 ? AiFullStatus : 0) | (aiCount_ >= 1 ? AiBusyStatus : 0);
            case 0x10: return aiDacRate_;
            case 0x14: return aiBitRate_;
        }
    } else if (inRange(a, PiRegs, 0x34)) {
        switch (a - PiRegs) {
            case 0x00: return piDramAddr_;
            case 0x04: return piCartAddr_;
            case 0x08:
            case 0x0C: return 0x7F;
            case 0x10: return piBusy_ ? 0x3u : 0u;  // DMA busy | IO busy
            default: {
                const unsigned index = (a - PiRegs - 0x14) >> 2;
                if (index < piDomain_.size()) return piDomain_[index];
            }
        }
    } else if (inRange(a, RiRegs, 0x20)) {
        return ri_[(a - RiRegs) >> 2];
    } else if (inRange(a, SiRegs, 0x1C)) {
        switch (a - SiRegs) {
            case 0x00: return siDramAddr_;
            case 0x04:
            case 0x10: return 0;
            case 0x18: return (siBusy_ ? 1u : 0u) | ((miIntr_ & IntrSI) ? (1u << 12) : 0u);
        }
    }
    ++unmappedAccesses_;
    return 0;
}

void MemoryBus::writeRegister(uint32_t a, uint32_t v) {
    if (inRange(a, SpRegs, 0x20)) {
        switch (a - SpRegs) {
            case 0x00: spMemAddr_ = v & 0x1FFF; return;
            case 0x04: spDramAddr_ = v & 0xFFFFFF; return;
            case 0x08: spDma(v, false); return;
            case 0x0C: spDma(v, true); return;
            case 0x10: writeSpStatus(v); return;
            case 0x1C: spSemaphore_ = 0; return;
            default: return;
        }
    } else if (a == SpPcReg) {
        spPc_ = v & 0xFFC;
        return;
    } else if (inRange(a, DpcRegs, 0x20)) {
        dpc_[(a - DpcRegs) >> 2] = v;
        return;
    } else if (inRange(a, DpsRegs, 0x10)) {
        dps_[(a - DpsRegs) >> 2] = v;
        return;
    } else if (inRange(a, MiRegs, 0x10)) {
        switch (a - MiRegs) {
            case 0x00:
                miMode_ = (miMode_ & ~0x7Fu) | (v & 0x7F);
                if (v & (1u << 7)) miMode_ &= ~0x80u;   // clear init mode
                if (v & (1u << 8)) miMode_ |= 0x80u;    // set init mode
                if (v & (1u << 11)) miIntr_ &= ~IntrDP;  // clear DP interrupt
                return;
            case 0x0C:
                // Each interrupt has a clear bit (2n) and a set bit (2n+1).
                for (unsigned n = 0; n < 6; ++n) {
                    if (v & (1u << (2 * n))) miMask_ &= ~(1u << n);
                    if (v & (1u << (2 * n + 1))) miMask_ |= (1u << n);
                }
                return;
            default: return;
        }
    } else if (inRange(a, ViRegsBase, 0x38)) {
        const unsigned index = (a - ViRegsBase) >> 2;
        if (index >= vi_.size()) return;
        switch (index) {
            case 0: vi_[0] = v & 0xFFFF; return;
            case 1: vi_[1] = v & 0xFFFFFF; return;
            case 2: vi_[2] = v & 0xFFF; return;
            case 3: vi_[3] = v & 0x3FF; return;
            case 4: miIntr_ &= ~IntrVI; return;  // writing V_CURRENT acknowledges the interrupt
            default: vi_[index] = v; return;
        }
    } else if (inRange(a, AiRegs, 0x18)) {
        switch (a - AiRegs) {
            case 0x00: aiDramAddr_ = v & 0xFFFFF8; return;
            case 0x04: {
                const uint32_t length = v & 0x3FFF8;
                if (length == 0 || aiCount_ >= aiQueue_.size()) return;
                aiQueue_[aiCount_++] = AudioBuffer{aiDramAddr_, length};
                if (aiCount_ == 1) startAudioBuffer(cycles_);
                return;
            }
            case 0x08: aiControl_ = v & 1; return;
            case 0x0C: miIntr_ &= ~IntrAI; return;
            case 0x10: aiDacRate_ = v & 0x3FFF; return;
            case 0x14: aiBitRate_ = v & 0xF; return;
            default: return;
        }
    } else if (inRange(a, PiRegs, 0x34)) {
        switch (a - PiRegs) {
            case 0x00: piDramAddr_ = v & 0xFFFFFF; return;
            case 0x04: piCartAddr_ = v; return;
            case 0x08:  // RDRAM -> cart: nothing is writable, just finish
                piBusy_ = true;
                piDoneCycle_ = cycles_ + PiBaseDelay;
                return;
            case 0x0C: piDmaFromCart(v); return;
            case 0x10:
                if (v & 1) piBusy_ = false;  // reset DMA controller
                if (v & 2) miIntr_ &= ~IntrPI;
                return;
            default: {
                const unsigned index = (a - PiRegs - 0x14) >> 2;
                if (index < piDomain_.size()) piDomain_[index] = v & 0xFF;
                return;
            }
        }
    } else if (inRange(a, RiRegs, 0x20)) {
        ri_[(a - RiRegs) >> 2] = v;
        return;
    } else if (inRange(a, SiRegs, 0x1C)) {
        switch (a - SiRegs) {
            case 0x00: siDramAddr_ = v & 0xFFFFFF; return;
            case 0x04: siDmaFromPif(); return;
            case 0x10: siDmaToPif(); return;
            case 0x18: miIntr_ &= ~IntrSI; return;  // any write acknowledges
            default: return;
        }
    }
    ++unmappedAccesses_;
}

// ---------------------------------------------------------------------------
// SP: DMA and status. There is no RSP core.
// ---------------------------------------------------------------------------

void MemoryBus::writeSpStatus(uint32_t v) {
    if (v & (1u << 1)) spStatus_ |= SpStatusHalt;
    if (v & (1u << 2)) spStatus_ &= ~SpStatusBroke;
    if (v & (1u << 3)) miIntr_ &= ~IntrSP;
    if (v & (1u << 4)) miIntr_ |= IntrSP;
    if (v & (1u << 5)) spStatus_ &= ~SpStatusSingleStep;
    if (v & (1u << 6)) spStatus_ |= SpStatusSingleStep;
    if (v & (1u << 7)) spStatus_ &= ~SpStatusIntrBreak;
    if (v & (1u << 8)) spStatus_ |= SpStatusIntrBreak;
    for (unsigned n = 0; n < 8; ++n) {
        if (v & (1u << (9 + 2 * n))) spStatus_ &= ~(SpStatusSignal0 << n);
        if (v & (1u << (10 + 2 * n))) spStatus_ |= (SpStatusSignal0 << n);
    }

    if (v & 1u) {
        // Clear-halt starts the RSP. With no RSP core the task is treated as
        // finishing immediately: halt, set "broke" and, if the game asked for
        // it, raise the SP interrupt.
        ++rspStartRequests_;
        spStatus_ |= SpStatusHalt | SpStatusBroke;
        if (spStatus_ & SpStatusIntrBreak) miIntr_ |= IntrSP;
    }
}

void MemoryBus::spDma(uint32_t lengthReg, bool toRdram) {
    const uint32_t length = ((lengthReg & 0xFFF) | 7) + 1;  // bytes per row, multiple of 8
    const uint32_t rows = ((lengthReg >> 12) & 0xFF) + 1;
    const uint32_t skip = (lengthReg >> 20) & 0xFFF;  // RDRAM-side gap between rows

    uint32_t mem = spMemAddr_ & 0x1FF8;
    uint32_t dram = spDramAddr_ & 0xFFFFF8;
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t i = 0; i < length; ++i) {
            // DMEM and IMEM are separate 4 KiB banks; a row wraps inside its bank.
            const uint32_t m = (mem & 0x1000) | ((mem + i) & 0xFFF);
            const uint64_t d = static_cast<uint64_t>(dram) + i;
            if (d >= RdramSize) continue;
            if (toRdram) rdram_[static_cast<size_t>(d)] = spMem_[m];
            else spMem_[m] = rdram_[static_cast<size_t>(d)];
        }
        mem = (mem & 0x1000) | ((mem + length) & 0xFFF);
        dram += length + skip;
    }
    spMemAddr_ = mem;
    spDramAddr_ = dram & 0xFFFFFF;
}

// ---------------------------------------------------------------------------
// PI / SI DMA
// ---------------------------------------------------------------------------

void MemoryBus::piDmaFromCart(uint32_t lengthReg) {
    uint32_t length = (lengthReg & 0xFFFFFF) + 1;
    length = (length + 1) & ~1u;  // transfers are rounded up to an even size

    const uint32_t dram = piDramAddr_ & 0xFFFFFE;
    const uint32_t cart = piCartAddr_;
    const std::vector<uint8_t>* rom = rom_ ? &rom_->data() : nullptr;

    for (uint32_t i = 0; i < length; ++i) {
        const uint64_t d = static_cast<uint64_t>(dram) + i;
        if (d >= RdramSize) break;
        uint8_t byte = 0;  // open bus / unmapped cart space reads as zero
        const uint64_t c = static_cast<uint64_t>(cart) + i;
        if (rom && c >= CartBase && c < CartEnd && c - CartBase < rom->size()) {
            byte = (*rom)[static_cast<size_t>(c - CartBase)];
        }
        rdram_[static_cast<size_t>(d)] = byte;
    }

    piBusy_ = true;
    piDoneCycle_ = cycles_ + PiBaseDelay + length;
}

void MemoryBus::siDmaToPif() {
    const uint32_t dram = siDramAddr_ & 0xFFFFF8;
    if (dram + PifRamSize <= RdramSize) {
        std::memcpy(pifRam_.data(), rdram_.data() + dram, PifRamSize);
    }
    // Bit 0 of the control byte asks the PIF to run the Joybus commands.
    if (pifRam_[PifRamSize - 1] & 0x01) {
        processPifCommands(pifRam_.data(), controllers_);
    }
    siBusy_ = true;
    siDoneCycle_ = cycles_ + SiDelay;
}

void MemoryBus::siDmaFromPif() {
    const uint32_t dram = siDramAddr_ & 0xFFFFF8;
    if (dram + PifRamSize <= RdramSize) {
        std::memcpy(rdram_.data() + dram, pifRam_.data(), PifRamSize);
    }
    siBusy_ = true;
    siDoneCycle_ = cycles_ + SiDelay;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

uint32_t MemoryBus::aiSampleRate() const { return AiDacClock / (aiDacRate_ + 1); }

uint64_t MemoryBus::aiBufferCycles(uint32_t length) const {
    const uint64_t frames = length / 4;  // 16-bit stereo
    const uint32_t rate = aiSampleRate();
    if (rate < 1000 || rate > 200000) return frames * 2000;  // DAC not configured sensibly
    return frames * CpuClockHz / rate;
}

void MemoryBus::startAudioBuffer(uint64_t startCycle) {
    const AudioBuffer& buffer = aiQueue_[0];
    aiEndCycle_ = startCycle + aiBufferCycles(buffer.length);

    const uint32_t rate = aiSampleRate();
    const uint32_t address = buffer.address & 0xFFFFFF;
    if (audioSink_ && rate >= 1000 && rate <= 200000 && address + buffer.length <= RdramSize) {
        audioSink_(rdram_.data() + address, buffer.length, rate);
    }
}

// ---------------------------------------------------------------------------
// Video timing and output
// ---------------------------------------------------------------------------

uint32_t MemoryBus::viLineCount() const {
    const uint32_t sync = vi_[6] & 0x3FF;
    return (sync >= 0x100 && sync <= 0x300) ? sync : defaultLines_;
}

uint64_t MemoryBus::viLineCycles() const {
    return std::max<uint64_t>(1, frameCycles_ / viLineCount());
}

void MemoryBus::stepVideo(uint64_t cycles) {
    const uint64_t lineCycles = viLineCycles();
    viLineProgress_ += cycles;
    if (viLineProgress_ < lineCycles) return;

    viLineProgress_ -= lineCycles;
    ++viLine_;
    if (viLine_ >= viLineCount()) {
        viLine_ = 0;
        ++frameCount_;
    }
    if (viLine_ == (vi_[3] & 0x3FF)) miIntr_ |= IntrVI;
}

bool MemoryBus::captureFrame(VideoFrame& out) const {
    ViRegisters regs;
    regs.control = vi_[0];
    regs.origin = vi_[1];
    regs.width = vi_[2];
    regs.hStart = vi_[9];
    regs.vStart = vi_[10];
    regs.xScale = vi_[12];
    regs.yScale = vi_[13];
    return decodeFramebuffer(regs, rdram_.data(), RdramSize, out);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

uint64_t MemoryBus::cyclesUntilNextEvent() const {
    uint64_t next = viLineCycles() - std::min(viLineProgress_, viLineCycles() - 1);
    const auto consider = [&](bool active, uint64_t when) {
        if (active) next = std::min(next, when > cycles_ ? when - cycles_ : 1);
    };
    consider(piBusy_, piDoneCycle_);
    consider(siBusy_, siDoneCycle_);
    consider(aiCount_ > 0, aiEndCycle_);
    return std::max<uint64_t>(1, next);
}

void MemoryBus::advance(uint64_t cycles) {
    while (cycles > 0) {
        const uint64_t step = std::min(cycles, cyclesUntilNextEvent());
        cycles -= step;
        cycles_ += step;
        stepVideo(step);

        if (piBusy_ && cycles_ >= piDoneCycle_) {
            piBusy_ = false;
            miIntr_ |= IntrPI;
        }
        if (siBusy_ && cycles_ >= siDoneCycle_) {
            siBusy_ = false;
            miIntr_ |= IntrSI;
        }
        while (aiCount_ > 0 && cycles_ >= aiEndCycle_) {
            // The playing buffer finished: the next queued one starts back to back.
            const uint64_t finishedAt = aiEndCycle_;
            aiQueue_[0] = aiQueue_[1];
            --aiCount_;
            miIntr_ |= IntrAI;
            if (aiCount_ > 0) startAudioBuffer(finishedAt);
        }
    }
}

}  // namespace reality64
