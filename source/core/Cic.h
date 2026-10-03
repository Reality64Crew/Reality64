#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace reality64 {

// The CIC lockout chip variant a cartridge was built for. The boot code ROM
// (IPL3) differs per variant, which changes where the game's entry point sits
// and which seed the PIF hands over.
enum class Cic { Unknown, Nus6101, Nus6102, Nus6103, Nus6105, Nus6106 };

uint32_t crc32(const uint8_t* data, size_t size);

// Identifies the CIC from the CRC32 of the IPL3 code (ROM bytes 0x40..0xFFF).
Cic detectCic(const std::vector<uint8_t>& rom);

const char* cicName(Cic cic);
// Value the PIF leaves in the s6 register for IPL3/the game.
uint32_t cicSeed(Cic cic);
// Offset subtracted from the header's boot address to get the real entry point.
uint32_t cicEntryAdjust(Cic cic);

}  // namespace reality64
