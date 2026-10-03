#pragma once

#include <cstdint>
#include <functional>

#include "input/Controller.h"

namespace reality64 {

using ControllerProvider = std::function<ControllerState(int port)>;

constexpr unsigned PifRamSize = 64;

// High-level emulation of the PIF's Joybus master. `pifRam` holds the 64-byte
// command block the game DMA'd in; every command is answered in place so a
// following read DMA returns the responses.
//
// Supported: controller info/reset (0x00/0xFF) and button state (0x01) on
// ports 1-4. Controller Paks and EEPROM are reported as absent.
void processPifCommands(uint8_t* pifRam, const ControllerProvider& provider);

}  // namespace reality64
