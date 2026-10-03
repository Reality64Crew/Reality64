#pragma once

#include <cstdint>
#include <vector>

namespace reality64 {

// A decoded frame, 4 bytes per pixel in memory order R, G, B, A.
struct VideoFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

// The VI registers that describe the output image.
struct ViRegisters {
    uint32_t control = 0;
    uint32_t origin = 0;
    uint32_t width = 0;
    uint32_t hStart = 0;
    uint32_t vStart = 0;
    uint32_t xScale = 0;
    uint32_t yScale = 0;
};

// Reads the framebuffer the VI is currently scanning out of RDRAM. Returns
// false when video output is off or the registers are not sensible yet.
bool decodeFramebuffer(const ViRegisters& vi, const uint8_t* rdram, uint32_t rdramSize, VideoFrame& out);

}  // namespace reality64
