#include "core/Video.h"

#include <algorithm>

namespace reality64 {

namespace {

constexpr uint32_t PixelFormatMask = 3;
constexpr uint32_t Format16 = 2;  // RGBA 5551
constexpr uint32_t Format32 = 3;  // RGBA 8888
constexpr uint32_t MaxWidth = 640;
constexpr uint32_t MaxHeight = 576;
constexpr uint32_t FallbackHeight = 240;

uint8_t expand5(uint32_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }

}  // namespace

bool decodeFramebuffer(const ViRegisters& vi, const uint8_t* rdram, uint32_t rdramSize, VideoFrame& out) {
    const uint32_t format = vi.control & PixelFormatMask;
    if (format != Format16 && format != Format32) return false;

    const uint32_t stride = vi.width & 0xFFF;
    if (stride == 0) return false;

    // Visible size comes from the display window (H/V_START) and the scalers:
    // a 640-wide window with X_SCALE 0.5 is a 320-pixel-wide image.
    const uint32_t hStart = (vi.hStart >> 16) & 0x3FF, hEnd = vi.hStart & 0x3FF;
    const uint32_t vStart = (vi.vStart >> 16) & 0x3FF, vEnd = vi.vStart & 0x3FF;
    uint32_t width = hEnd > hStart ? ((hEnd - hStart) * (vi.xScale & 0xFFF)) >> 10 : 0;
    if (width == 0 || width > stride) width = stride;
    width = std::min(width, MaxWidth);

    uint32_t height = vEnd > vStart ? (((vEnd - vStart) >> 1) * (vi.yScale & 0xFFF)) >> 10 : 0;
    // The window is a few lines short of the nominal 240/480; round up.
    height = (height + 7) & ~7u;
    if (height == 0 || height > MaxHeight) height = FallbackHeight;

    const uint32_t bytesPerPixel = format == Format16 ? 2 : 4;
    const uint32_t origin = vi.origin & 0xFFFFFF;
    const uint64_t needed = static_cast<uint64_t>(origin) +
                            static_cast<uint64_t>(height - 1) * stride * bytesPerPixel +
                            static_cast<uint64_t>(width) * bytesPerPixel;
    if (needed > rdramSize) return false;

    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<size_t>(width) * height * 4);

    uint8_t* dst = out.rgba.data();
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = rdram + origin + static_cast<size_t>(y) * stride * bytesPerPixel;
        for (uint32_t x = 0; x < width; ++x) {
            if (format == Format16) {
                const uint32_t v = (uint32_t(src[0]) << 8) | src[1];
                dst[0] = expand5((v >> 11) & 31);
                dst[1] = expand5((v >> 6) & 31);
                dst[2] = expand5((v >> 1) & 31);
                src += 2;
            } else {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                src += 4;
            }
            dst[3] = 255;
            dst += 4;
        }
    }
    return true;
}

}  // namespace reality64
