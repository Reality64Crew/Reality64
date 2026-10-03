#pragma once

#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#include <stdlib.h>
#endif

namespace reality64 {

// The N64 is big-endian. These load/store big-endian values from raw memory
// with a single (byte-swapped) access on little-endian hosts.

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define REALITY64_BIG_ENDIAN_HOST 1
#endif

inline uint16_t swap16(uint16_t v) {
#if defined(REALITY64_BIG_ENDIAN_HOST)
    return v;
#elif defined(_MSC_VER)
    return _byteswap_ushort(v);
#else
    return __builtin_bswap16(v);
#endif
}

inline uint32_t swap32(uint32_t v) {
#if defined(REALITY64_BIG_ENDIAN_HOST)
    return v;
#elif defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}

inline uint64_t swap64(uint64_t v) {
#if defined(REALITY64_BIG_ENDIAN_HOST)
    return v;
#elif defined(_MSC_VER)
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}

inline uint16_t loadBe16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return swap16(v);
}
inline uint32_t loadBe32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return swap32(v);
}
inline uint64_t loadBe64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return swap64(v);
}
inline void storeBe16(uint8_t* p, uint16_t v) {
    v = swap16(v);
    std::memcpy(p, &v, sizeof v);
}
inline void storeBe32(uint8_t* p, uint32_t v) {
    v = swap32(v);
    std::memcpy(p, &v, sizeof v);
}
inline void storeBe64(uint8_t* p, uint64_t v) {
    v = swap64(v);
    std::memcpy(p, &v, sizeof v);
}

}  // namespace reality64
