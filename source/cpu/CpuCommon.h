#pragma once

#include <cstdint>

// Constants and helpers shared by the VR4300 translation units.
namespace reality64::cpu {

constexpr uint32_t StatusIE = 1u << 0;
constexpr uint32_t StatusEXL = 1u << 1;
constexpr uint32_t StatusERL = 1u << 2;
constexpr uint32_t StatusIM = 0xFF00;
constexpr uint32_t StatusBEV = 1u << 22;
constexpr uint32_t StatusFR = 1u << 26;
constexpr uint32_t StatusCU1 = 1u << 29;

constexpr uint32_t CauseBD = 1u << 31;
constexpr uint32_t CauseIP2 = 1u << 10;  // MI interrupt line
constexpr uint32_t CauseIP7 = 1u << 15;  // Count == Compare
constexpr uint32_t CauseIPMask = 0xFF00;
constexpr uint32_t CauseSoftwareIP = 0x0300;  // IP0/IP1 are the only writable bits
constexpr uint32_t CauseExcMask = 0x7Cu;
constexpr uint32_t CauseCEMask = 3u << 28;

constexpr uint32_t FcsrRoundMask = 3;
constexpr uint32_t FcsrCondition = 1u << 23;
constexpr uint32_t FcsrWritable = 0x0183FFFFu;

constexpr unsigned TlbEntries = 32;

constexpr uint64_t sx32(uint32_t v) {
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v)));
}
constexpr uint64_t sx16(uint32_t v) {
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(v)));
}

}  // namespace reality64::cpu
