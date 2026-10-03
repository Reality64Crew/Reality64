#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace reality64 {

// Minimal binary serialisation for save states. Integers are little-endian
// regardless of the host, so a state file is portable between platforms.
class StateWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void bytes(const void* data, size_t size) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        buf_.insert(buf_.end(), p, p + size);
    }
    void string(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        bytes(s.data(), s.size());
    }
    template <typename Array>
    void u32s(const Array& a) {
        for (uint32_t v : a) u32(v);
    }
    template <typename Array>
    void u64s(const Array& a) {
        for (uint64_t v : a) u64(v);
    }

    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t> take() { return std::move(buf_); }

private:
    std::vector<uint8_t> buf_;
};

// Reads what StateWriter wrote. Reading past the end sets a sticky failure
// flag and yields zeros, so callers can read a whole structure and check ok()
// once at the end.
class StateReader {
public:
    StateReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint8_t u8() { return need(1) ? data_[pos_++] : 0; }
    bool boolean() { return u8() != 0; }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data_[pos_++]) << (8 * i);
        return v;
    }
    uint64_t u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data_[pos_++]) << (8 * i);
        return v;
    }
    void bytes(void* out, size_t size) {
        if (!need(size)) return;
        std::copy(data_ + pos_, data_ + pos_ + size, static_cast<uint8_t*>(out));
        pos_ += size;
    }
    std::string string() {
        const uint32_t n = u32();
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }
    template <typename Array>
    void u32s(Array& a) {
        for (auto& v : a) v = u32();
    }
    template <typename Array>
    void u64s(Array& a) {
        for (auto& v : a) v = u64();
    }

    bool ok() const { return ok_; }
    size_t remaining() const { return size_ - pos_; }

private:
    bool need(size_t n) {
        if (!ok_ || n > size_ - pos_) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace reality64
