#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace parties {

class BinaryWriter {
public:
    void write_u8(uint8_t val);
    void write_u16(uint16_t val);
    void write_u32(uint32_t val);
    void write_u64(uint64_t val);
    void write_string(const std::string& str);
    void write_bytes(const uint8_t* data, size_t len);

    const std::vector<uint8_t>& data() const { return buf_; }
    size_t size() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t len);

    uint8_t read_u8();
    uint16_t read_u16();
    uint32_t read_u32();
    uint64_t read_u64();
    std::string read_string();
    void read_bytes(uint8_t* out, size_t len);

    template <class T>
    void skip() {
        if constexpr (std::is_same_v<T, std::string>) {
            skip_bytes(read_u16());
        } else {
            static_assert(std::has_unique_object_representations_v<T>,
                          "skip<T> needs a fixed-width wire field");
            skip_bytes(sizeof(T));
        }
    }

    bool has_remaining(size_t bytes) const;
    size_t remaining() const;
    bool error() const { return error_; }

private:
    void skip_bytes(size_t len);

    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
    bool error_ = false;
};

}
