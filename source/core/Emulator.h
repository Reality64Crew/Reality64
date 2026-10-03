#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/Cic.h"
#include "core/MemoryBus.h"
#include "core/Rom.h"
#include "cpu/VR4300.h"

namespace reality64 {

// Owns the machine: cartridge, memory bus and CPU.
class Emulator {
public:
    Emulator() : cpu_(bus_) {}
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

    MemoryBus& bus() { return bus_; }
    VR4300& cpu() { return cpu_; }
    const Rom* rom() const { return rom_.get(); }
    Cic cic() const { return cic_; }
    bool isPal() const { return pal_; }

private:
    uint64_t execute(uint64_t maxSteps, bool untilFrame);

    MemoryBus bus_;
    VR4300 cpu_;
    std::shared_ptr<Rom> rom_;
    Cic cic_ = Cic::Unknown;
    bool pal_ = false;
};

}  // namespace reality64
