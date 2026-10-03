#include "core/Rom.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace reality64 {

namespace {

constexpr uint32_t MagicBigEndian = 0x80371240;
constexpr uint32_t MagicByteSwapped = 0x37804012;
constexpr uint32_t MagicLittleEndian = 0x40123780;

uint32_t readBe32(const std::vector<uint8_t>& d, size_t off) {
    return (uint32_t(d[off]) << 24) | (uint32_t(d[off + 1]) << 16) |
           (uint32_t(d[off + 2]) << 8) | uint32_t(d[off + 3]);
}

std::string readText(const std::vector<uint8_t>& d, size_t off, size_t len) {
    std::string s(d.begin() + off, d.begin() + off + len);
    // Titles are space/NUL padded and sometimes contain stray non-ASCII bytes.
    for (char& c : s) {
        if (c != '\0' && (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)) {
            c = '?';
        }
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) {
        s.pop_back();
    }
    return s;
}

}  // namespace

const char* romFormatName(RomFormat format) {
    switch (format) {
        case RomFormat::BigEndian: return "z64 (big-endian)";
        case RomFormat::ByteSwapped: return "v64 (byte-swapped)";
        case RomFormat::LittleEndian: return "n64 (little-endian)";
    }
    return "unknown";
}

bool Rom::loadFromFile(const std::string& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open '" + path + "'";
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return loadFromMemory(std::move(bytes), error);
}

bool Rom::loadFromMemory(std::vector<uint8_t> data, std::string& error) {
    if (data.size() < BootCodeSize) {
        error = "file is too small to be an N64 ROM (" + std::to_string(data.size()) + " bytes)";
        return false;
    }

    // Byte-order swaps work on whole words, so pad a truncated dump.
    while (data.size() % 4 != 0) {
        data.push_back(0);
    }

    const uint32_t magic = readBe32(data, 0);
    switch (magic) {
        case MagicBigEndian:
            format_ = RomFormat::BigEndian;
            break;
        case MagicByteSwapped:
            format_ = RomFormat::ByteSwapped;
            for (size_t i = 0; i < data.size(); i += 2) {
                std::swap(data[i], data[i + 1]);
            }
            break;
        case MagicLittleEndian:
            format_ = RomFormat::LittleEndian;
            for (size_t i = 0; i < data.size(); i += 4) {
                std::swap(data[i], data[i + 3]);
                std::swap(data[i + 1], data[i + 2]);
            }
            break;
        default:
            error = "not an N64 ROM (unrecognised header magic)";
            return false;
    }

    data_ = std::move(data);
    parseHeader();
    return true;
}

void Rom::parseHeader() {
    header_.clockRate = readBe32(data_, 0x04);
    header_.bootAddress = readBe32(data_, 0x08);
    header_.release = readBe32(data_, 0x0C);
    header_.crc1 = readBe32(data_, 0x10);
    header_.crc2 = readBe32(data_, 0x14);
    header_.name = readText(data_, 0x20, 20);
    header_.gameCode = readText(data_, 0x3B, 4);
    header_.version = data_[0x3F];
}

}  // namespace reality64
