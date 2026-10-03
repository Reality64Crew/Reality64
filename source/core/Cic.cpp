#include "core/Cic.h"

namespace reality64 {

uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// CRC32 values of the known IPL3 images. These are written from memory and have
// not been checked against real cartridge dumps yet; an image that does not
// match falls back to Unknown, which boots like a 6102.
Cic detectCic(const std::vector<uint8_t>& rom) {
    if (rom.size() < 0x1000) return Cic::Unknown;
    switch (crc32(rom.data() + 0x40, 0x1000 - 0x40)) {
        case 0x6170A4A1u: return Cic::Nus6101;
        case 0x90BB6CB5u: return Cic::Nus6102;
        case 0x0B050EE0u: return Cic::Nus6103;
        case 0x98BC2C86u: return Cic::Nus6105;
        case 0xACC8580Au: return Cic::Nus6106;
        default: return Cic::Unknown;
    }
}

const char* cicName(Cic cic) {
    switch (cic) {
        case Cic::Nus6101: return "CIC-NUS-6101";
        case Cic::Nus6102: return "CIC-NUS-6102";
        case Cic::Nus6103: return "CIC-NUS-6103";
        case Cic::Nus6105: return "CIC-NUS-6105";
        case Cic::Nus6106: return "CIC-NUS-6106";
        case Cic::Unknown: break;
    }
    return "unknown (assuming 6102)";
}

uint32_t cicSeed(Cic cic) {
    switch (cic) {
        case Cic::Nus6103: return 0x78;
        case Cic::Nus6105: return 0x91;
        case Cic::Nus6106: return 0x85;
        default: return 0x3F;
    }
}

uint32_t cicEntryAdjust(Cic cic) {
    switch (cic) {
        case Cic::Nus6103: return 0x100000;
        case Cic::Nus6106: return 0x200000;
        default: return 0;
    }
}

}  // namespace reality64
