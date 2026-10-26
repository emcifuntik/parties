#include <parties/serialization.h>
#include <cstring>

namespace parties {

void BinaryWriter::write_u8(uint8_t val) {
    buf_.push_back(val);
}

void BinaryWriter::write_u16(uint16_t val) {
    buf_.push_back(static_cast<uint8_t>(val & 0xFF));
    buf_.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

void BinaryWriter::write_u32(uint32_t val) {
    buf_.push_back(static_cast<uint8_t>(val & 0xFF));
    buf_.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf_.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf_.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

void BinaryWriter::write_u64(uint64_t val) {
    for (int i = 0; i < 8; i++)
        buf_.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
}

void BinaryWriter::write_string(const std::string& str) {
    write_u16(static_cast<uint16_t>(str.size()));
    write_bytes(reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

void BinaryWriter::write_bytes(const uint8_t* data, size_t len) {
    buf_.insert(buf_.end(), data, data + len);
}

BinaryReader::BinaryReader(const uint8_t* data, size_t len)
    : data_(data), len_(len) {}

uint8_t BinaryReader::read_u8() {
    if (!has_remaining(1)) { error_ = true; return 0; }
    return data_[pos_++];
}

uint16_t BinaryReader::read_u16() {
    if (!has_remaining(2)) { error_ = true; return 0; }
    uint16_t val = static_cast<uint16_t>(data_[pos_])
                 | (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return val;
}

uint32_t BinaryReader::read_u32() {
    if (!has_remaining(4)) { error_ = true; return 0; }
    uint32_t val = static_cast<uint32_t>(data_[pos_])
                 | (static_cast<uint32_t>(data_[pos_ + 1]) << 8)
                 | (static_cast<uint32_t>(data_[pos_ + 2]) << 16)
                 | (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
    pos_ += 4;
    return val;
}

uint64_t BinaryReader::read_u64() {
    if (!has_remaining(8)) { error_ = true; return 0; }
    uint64_t val = 0;
    for (int i = 0; i < 8; i++)
        val |= static_cast<uint64_t>(data_[pos_ + i]) << (i * 8);
    pos_ += 8;
    return val;
}

std::string BinaryReader::read_string() {
    uint16_t len = read_u16();
    if (error_ || !has_remaining(len)) { error_ = true; return ""; }
    std::string str(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    return str;
}

void BinaryReader::read_bytes(uint8_t* out, size_t len) {
    if (!has_remaining(len)) { error_ = true; return; }
    std::memcpy(out, data_ + pos_, len);
    pos_ += len;
}

void BinaryReader::skip_bytes(size_t len) {
    if (!has_remaining(len)) { error_ = true; return; }
    pos_ += len;
}

bool BinaryReader::has_remaining(size_t bytes) const {
    return pos_ + bytes <= len_;
}

size_t BinaryReader::remaining() const {
    return len_ - pos_;
}

}
