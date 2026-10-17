#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "oal/protocol.hpp"

namespace oal {

class ByteWriter {
public:
    void put_u8(std::uint8_t v) { bytes_.push_back(v); }

    void put_u16(std::uint16_t v) {
        bytes_.push_back(static_cast<std::uint8_t>(v & 0xFFU));
        bytes_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
    }

    void put_u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            bytes_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
        }
    }

    void put_u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            bytes_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
        }
    }

    void put_float(float v) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        put_u32(bits);
    }

    void put_bytes(const void* data, std::size_t length) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes_.insert(bytes_.end(), p, p + length);
    }

    std::size_t size() const { return bytes_.size(); }
    const std::vector<std::uint8_t>& data() const { return bytes_; }
    std::vector<std::uint8_t>& data() { return bytes_; }
    void clear() { bytes_.clear(); }

private:
    std::vector<std::uint8_t> bytes_;
};

class ByteReader {
public:
    ByteReader(const void* data, std::size_t length)
        : ptr_(static_cast<const std::uint8_t*>(data)), size_(length) {}

    bool ok() const { return ok_; }
    std::size_t remaining() const { return ok_ ? (size_ - pos_) : 0; }

    bool get_u8(std::uint8_t& v) {
        if (remaining() < 1) {
            return fail();
        }
        v = ptr_[pos_++];
        return true;
    }

    bool get_u16(std::uint16_t& v) {
        if (remaining() < 2) {
            return fail();
        }
        v = static_cast<std::uint16_t>(ptr_[pos_]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(ptr_[pos_ + 1]) << 8);
        pos_ += 2;
        return true;
    }

    bool get_u32(std::uint32_t& v) {
        if (remaining() < 4) {
            return fail();
        }
        v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(ptr_[pos_ + static_cast<std::size_t>(i)]) << (8 * i);
        }
        pos_ += 4;
        return true;
    }

    bool get_u64(std::uint64_t& v) {
        if (remaining() < 8) {
            return fail();
        }
        v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(ptr_[pos_ + static_cast<std::size_t>(i)]) << (8 * i);
        }
        pos_ += 8;
        return true;
    }

    bool get_float(float& v) {
        std::uint32_t bits = 0;
        if (!get_u32(bits)) {
            return false;
        }
        std::memcpy(&v, &bits, sizeof(v));
        return true;
    }

    bool get_bytes(void* out, std::size_t length) {
        if (remaining() < length) {
            return fail();
        }
        std::memcpy(out, ptr_ + pos_, length);
        pos_ += length;
        return true;
    }

private:
    bool fail() {
        ok_ = false;
        return false;
    }

    const std::uint8_t* ptr_;
    std::size_t size_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

struct Frame {
    FrameHeader header;
    std::vector<std::uint8_t> payload;

    std::size_t size() const { return kHeaderSize + payload.size(); }
};

void encode_header(const FrameHeader& header, ByteWriter& out);
bool decode_header(ByteReader& in, FrameHeader& header);

bool encode_control(const ControlPayload& msg, std::vector<std::uint8_t>& out);
bool decode_control(const void* data, std::size_t length, ControlPayload& out);

bool encode_telemetry(const TelemetryPayload& msg, std::vector<std::uint8_t>& out);
bool decode_telemetry(const void* data, std::size_t length, TelemetryPayload& out);

bool encode_debug(const DebugPayload& msg, std::vector<std::uint8_t>& out);
bool decode_debug(const void* data, std::size_t length, DebugPayload& out);

void encode_frame(Frame& frame, std::vector<std::uint8_t>& out);
bool decode_frame(const void* data, std::size_t length, Frame& out, std::string* error = nullptr);

std::string stream_name(std::uint8_t stream);

}  // namespace oal
