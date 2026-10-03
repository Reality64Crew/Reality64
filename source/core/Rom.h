#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace reality64 {

// Byte order of a cartridge dump, detected from the first word of the header.
enum class RomFormat {
    BigEndian,    // .z64 - native N64 order
    ByteSwapped,  // .v64 - every pair of bytes swapped
    LittleEndian  // .n64 - every 32-bit word reversed
};

struct RomHeader {
    uint32_t clockRate = 0;
    uint32_t bootAddress = 0;
    uint32_t release = 0;
    uint32_t crc1 = 0;
    uint32_t crc2 = 0;
    std::string name;
    std::string gameCode;
    uint8_t version = 0;
};

class Rom {
public:
    static constexpr size_t HeaderSize = 0x40;
    static constexpr size_t BootCodeSize = 0x1000;

    bool loadFromFile(const std::string& path, std::string& error);
    // Takes a raw dump in any supported byte order and normalises it to big-endian.
    bool loadFromMemory(std::vector<uint8_t> data, std::string& error);

    RomFormat originalFormat() const { return format_; }
    const RomHeader& header() const { return header_; }
    const std::vector<uint8_t>& data() const { return data_; }

private:
    void parseHeader();

    RomFormat format_ = RomFormat::BigEndian;
    RomHeader header_;
    std::vector<uint8_t> data_;
};

const char* romFormatName(RomFormat format);

}  // namespace reality64
