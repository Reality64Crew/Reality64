#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Cic.h"
#include "core/MemoryBus.h"
#include "core/Rom.h"
#include "cpu/VR4300.h"

namespace reality64 {

// Owns the machine: cartridge, memory bus and CPU.
class Emulator {
public:
    Emulator() : cpu_(bus_) { cpu_.setCyclesPerInstruction(DefaultCyclesPerInstruction); }
    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;

    bool loadRom(const std::string& path, std::string& error);
    // Skips the PIF/IPL3 boot ROMs and sets up the state they would leave behind.
    bool boot(std::string& error);

    // Runs until the CPU halts or maxSteps instructions have executed.
    // Hardware time advances alongside, so interrupts and DMA happen.
    uint64_t run(uint64_t maxSteps);
    // Runs until the VI finishes the current frame (or the CPU halts).
    // Returns the number of instructions executed.
    uint64_t runFrame();

    // Whole-machine snapshot. A state only loads into the same ROM it came from.
    std::vector<uint8_t> saveState() const;
    bool loadState(const std::vector<uint8_t>& data, std::string& error);

    MemoryBus& bus() { return bus_; }
    VR4300& cpu() { return cpu_; }
    const Rom* rom() const { return rom_.get(); }
    Cic cic() const { return cic_; }
    bool isPal() const { return pal_; }

    // Average clock cycles charged per instruction (see VR4300). Lower is more
    // demanding on the host; 2 is a reasonable middle ground.
    static constexpr unsigned DefaultCyclesPerInstruction = 2;
    void setCyclesPerInstruction(unsigned cycles) { cpu_.setCyclesPerInstruction(cycles); }
    unsigned cyclesPerInstruction() const { return cpu_.cyclesPerInstruction(); }
    // Runs the video at this refresh rate regardless of the game region (0 =
    // the region default). PAL games then run faster than on real hardware.
    // Takes effect at the next boot().
    void setForcedRefresh(unsigned hz) { forcedRefreshHz_ = hz; }

private:
    uint64_t execute(uint64_t maxSteps, bool untilFrame);

    MemoryBus bus_;
    VR4300 cpu_;
    std::shared_ptr<Rom> rom_;
    Cic cic_ = Cic::Unknown;
    bool pal_ = false;
    unsigned forcedRefreshHz_ = 0;
};

}  // namespace reality64
