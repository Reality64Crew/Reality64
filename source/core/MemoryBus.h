#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "core/Endian.h"
#include "core/Pif.h"
#include "core/StateIO.h"
#include "core/Rom.h"
#include "core/Video.h"

namespace reality64 {

// Physical address space of the N64 plus the memory-mapped hardware registers.
// All multi-byte accesses are big-endian, independent of the host.
//
// Modelled: RDRAM, SP DMEM/IMEM, cartridge ROM, PIF RAM and the MI, VI, AI, PI,
// SI, RI and SP register blocks, including DMA, interrupts and timing.
// The RSP and RDP are not emulated: starting the RSP completes a "null task"
// at once (halt + break + interrupt) so software does not deadlock, and the
// RDP register block only stores values.
class MemoryBus {
public:
    static constexpr uint32_t RdramSize = 8 * 1024 * 1024;  // 4 MiB + Expansion Pak
    static constexpr uint32_t SpMemBase = 0x04000000;       // DMEM, then IMEM
    static constexpr uint32_t SpMemSize = 0x2000;
    static constexpr uint32_t CartBase = 0x10000000;
    static constexpr uint32_t CartEnd = 0x1FC00000;
    static constexpr uint32_t PifRamBase = 0x1FC007C0;

    static constexpr uint64_t CpuClockHz = 93750000;

    // Bits of MI_INTR / MI_INTR_MASK.
    static constexpr uint32_t IntrSP = 0x01;
    static constexpr uint32_t IntrSI = 0x02;
    static constexpr uint32_t IntrAI = 0x04;
    static constexpr uint32_t IntrVI = 0x08;
    static constexpr uint32_t IntrPI = 0x10;
    static constexpr uint32_t IntrDP = 0x20;

    // Receives big-endian 16-bit stereo audio when the game starts an AI DMA.
    using AudioSink = std::function<void(const uint8_t* samples, uint32_t bytes, uint32_t sampleRate)>;

    MemoryBus();

    void attachRom(std::shared_ptr<const Rom> rom) { rom_ = std::move(rom); }
    const Rom* rom() const { return rom_.get(); }

    // size must be 1, 2, 4 or 8 and paddr naturally aligned.
    // Unmapped reads return 0 and unmapped/read-only writes are dropped; both
    // are counted so callers can tell that the guest touched something missing.
    // RDRAM, where almost every access lands, is handled inline.
    uint64_t read(uint32_t paddr, unsigned size) {
        if (paddr < RdramSize && size <= RdramSize - paddr) {
            const uint8_t* p = rdram_.data() + paddr;
            switch (size) {
                case 4: return loadBe32(p);
                case 1: return p[0];
                case 2: return loadBe16(p);
                case 8: return loadBe64(p);
                default: break;
            }
        }
        return readSlow(paddr, size);
    }
    void write(uint32_t paddr, unsigned size, uint64_t value) {
        if (paddr < RdramSize && size <= RdramSize - paddr) {
            uint8_t* p = rdram_.data() + paddr;
            switch (size) {
                case 4: storeBe32(p, static_cast<uint32_t>(value)); return;
                case 1: p[0] = static_cast<uint8_t>(value); return;
                case 2: storeBe16(p, static_cast<uint16_t>(value)); return;
                case 8: storeBe64(p, value); return;
                default: break;
            }
        }
        writeSlow(paddr, size, value);
    }

    uint32_t read32(uint32_t paddr) { return static_cast<uint32_t>(read(paddr, 4)); }
    void write32(uint32_t paddr, uint32_t value) { write(paddr, 4, value); }

    uint8_t* rdram() { return rdram_.data(); }
    uint8_t* spMem() { return spMem_.data(); }
    uint8_t* pifRam() { return pifRam_.data(); }

    // Power-on state of the hardware registers (also done by the constructor).
    void resetDevices();
    // Register values IPL3 leaves behind; used by the high-level boot.
    void applyBootRegisterState();

    // --- time and interrupts ---
    // Advances hardware time; the CPU calls this after running `cycles` cycles.
    void advance(uint64_t cycles);
    // An upper bound for how long the CPU may run before hardware needs service.
    uint64_t cyclesUntilNextEvent() const;
    bool interruptPending() const { return (miIntr_ & miMask_) != 0; }
    uint64_t frameCount() const { return frameCount_; }
    uint64_t cycles() const { return cycles_; }

    // refreshHz overrides the region default (50 for PAL, 60 for NTSC) when non-zero.
    void setVideoStandard(bool pal, unsigned refreshHz = 0);
    double frameRate() const { return static_cast<double>(CpuClockHz) / static_cast<double>(frameCycles_); }

    // Decodes the framebuffer the VI is displaying. False if video is off.
    bool captureFrame(VideoFrame& out) const;

    void setControllerProvider(ControllerProvider provider) { controllers_ = std::move(provider); }
    void setAudioSink(AudioSink sink) { audioSink_ = std::move(sink); }

    uint64_t unmappedAccesses() const { return unmappedAccesses_; }

    // Everything but the cartridge and the host callbacks.
    void saveState(StateWriter& w) const;
    void loadState(StateReader& r);
    uint64_t rspStartRequests() const { return rspStartRequests_; }

private:
    struct AudioBuffer {
        uint32_t address = 0;
        uint32_t length = 0;
    };

    uint64_t readSlow(uint32_t paddr, unsigned size);
    void writeSlow(uint32_t paddr, unsigned size, uint64_t value);
    const uint8_t* mapRead(uint32_t paddr, unsigned size) const;
    uint8_t* mapWrite(uint32_t paddr, unsigned size);

    uint64_t readMmio(uint32_t paddr, unsigned size);
    void writeMmio(uint32_t paddr, unsigned size, uint64_t value);
    uint32_t readRegister(uint32_t addr);
    void writeRegister(uint32_t addr, uint32_t value);

    void writeSpStatus(uint32_t value);
    void spDma(uint32_t lengthReg, bool toRdram);
    void piDmaFromCart(uint32_t lengthReg);
    void siDmaToPif();
    void siDmaFromPif();
    void startAudioBuffer(uint64_t startCycle);
    uint32_t aiSampleRate() const;
    uint64_t aiBufferCycles(uint32_t length) const;
    uint32_t viLineCount() const;
    uint64_t viLineCycles() const;
    uint32_t viCurrentLine() const { return viLine_; }
    void stepVideo(uint64_t cycles);

    std::vector<uint8_t> rdram_;
    std::vector<uint8_t> spMem_;
    std::array<uint8_t, PifRamSize> pifRam_{};
    std::shared_ptr<const Rom> rom_;
    ControllerProvider controllers_;
    AudioSink audioSink_;
    uint64_t unmappedAccesses_ = 0;
    uint64_t rspStartRequests_ = 0;

    // SP
    uint32_t spMemAddr_ = 0, spDramAddr_ = 0, spStatus_ = 0, spSemaphore_ = 0, spPc_ = 0;
    // DPC / DPS: stored only
    std::array<uint32_t, 8> dpc_{};
    std::array<uint32_t, 4> dps_{};
    // MI
    uint32_t miMode_ = 0, miIntr_ = 0, miMask_ = 0;
    // VI: control, origin, width, v_intr, v_current, timing, v_sync, h_sync, leap,
    //     h_start, v_start, v_burst, x_scale, y_scale
    std::array<uint32_t, 14> vi_{};
    // AI
    uint32_t aiDramAddr_ = 0, aiControl_ = 0, aiDacRate_ = 0, aiBitRate_ = 0;
    std::array<AudioBuffer, 2> aiQueue_{};
    unsigned aiCount_ = 0;  // entry 0 is the one playing
    uint64_t aiEndCycle_ = 0;
    // PI
    uint32_t piDramAddr_ = 0, piCartAddr_ = 0;
    std::array<uint32_t, 8> piDomain_{};
    bool piBusy_ = false;
    uint64_t piDoneCycle_ = 0;
    // RI
    std::array<uint32_t, 8> ri_{};
    // SI
    uint32_t siDramAddr_ = 0;
    bool siBusy_ = false;
    uint64_t siDoneCycle_ = 0;

    // time
    uint64_t cycles_ = 0;
    uint64_t frameCycles_ = CpuClockHz / 60;
    uint32_t defaultLines_ = 525;
    uint32_t viLine_ = 0;
    uint64_t viLineProgress_ = 0;
    uint64_t frameCount_ = 0;
};

}  // namespace reality64
