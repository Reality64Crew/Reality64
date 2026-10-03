#pragma once

#include <cstdint>

namespace reality64 {

// Bit layout of the 16 button bits the N64 controller reports over Joybus.
namespace N64Button {
constexpr uint16_t A = 0x8000;
constexpr uint16_t B = 0x4000;
constexpr uint16_t Z = 0x2000;
constexpr uint16_t Start = 0x1000;
constexpr uint16_t DUp = 0x0800;
constexpr uint16_t DDown = 0x0400;
constexpr uint16_t DLeft = 0x0200;
constexpr uint16_t DRight = 0x0100;
constexpr uint16_t L = 0x0020;
constexpr uint16_t R = 0x0010;
constexpr uint16_t CUp = 0x0008;
constexpr uint16_t CDown = 0x0004;
constexpr uint16_t CLeft = 0x0002;
constexpr uint16_t CRight = 0x0001;
}  // namespace N64Button

constexpr int MaxControllerPorts = 4;
// The stick physically reaches about +/-85 on a real controller.
constexpr int StickMax = 85;

struct ControllerState {
    bool connected = false;
    uint16_t buttons = 0;
    int8_t stickX = 0;  // right is positive
    int8_t stickY = 0;  // up is positive
};

}  // namespace reality64
