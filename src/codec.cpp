#include "oal/codec.hpp"

#include <array>

#include "oal/crc.hpp"

namespace oal {

void encode_header(const FrameHeader& header, ByteWriter& out) {
    out.put_u32(header.magic);
    out.put_u8(header.version);
    out.put_u8(header.stream);
    out.put_u16(header.flags);
    out.put_u32(header.seq);
    out.put_u64(header.timestamp_ns);
    out.put_u16(header.payload_len);
    out.put_u16(header.reserved);
    out.put_u32(header.payload_crc);
}

bool decode_header(ByteReader& in, FrameHeader& header) {
    return in.get_u32(header.magic) && in.get_u8(header.version) && in.get_u8(header.stream) &&
           in.get_u16(header.flags) && in.get_u32(header.seq) && in.get_u64(header.timestamp_ns) &&
           in.get_u16(header.payload_len) && in.get_u16(header.reserved) &&
           in.get_u32(header.payload_crc);
}

bool encode_control(const ControlPayload& msg, std::vector<std::uint8_t>& out) {
    ByteWriter w;
    for (float a : msg.axes) {
        w.put_float(a);
    }
    w.put_u16(msg.buttons);
    w.put_u8(msg.mode);
    w.put_u8(msg.flags);
    for (float p : msg.params) {
        w.put_float(p);
    }
    out = w.data();
    return out.size() == kControlPayloadBytes;
}

bool decode_control(const void* data, std::size_t length, ControlPayload& out) {
    if (length < kControlPayloadBytes) {
        return false;
    }
    ByteReader r(data, length);
    for (float& a : out.axes) {
        if (!r.get_float(a)) {
            return false;
        }
    }
    if (!r.get_u16(out.buttons) || !r.get_u8(out.mode) || !r.get_u8(out.flags)) {
        return false;
    }
    for (float& p : out.params) {
        if (!r.get_float(p)) {
            return false;
        }
    }
    return r.ok();
}

bool encode_telemetry(const TelemetryPayload& msg, std::vector<std::uint8_t>& out) {
    ByteWriter w;
    for (const auto& q : msg.quaternions) {
        for (float v : q) {
            w.put_float(v);
        }
    }
    for (float v : msg.lasers) {
        w.put_float(v);
    }
    for (float v : msg.state) {
        w.put_float(v);
    }
    out = w.data();
    return out.size() == kTelemetryPayloadBytes;
}

bool decode_telemetry(const void* data, std::size_t length, TelemetryPayload& out) {
    if (length < kTelemetryPayloadBytes) {
        return false;
    }
    ByteReader r(data, length);
    for (auto& q : out.quaternions) {
        for (float& v : q) {
            if (!r.get_float(v)) {
                return false;
            }
        }
    }
    for (float& v : out.lasers) {
        if (!r.get_float(v)) {
            return false;
        }
    }
    for (float& v : out.state) {
        if (!r.get_float(v)) {
            return false;
        }
    }
    return r.ok();
}

bool encode_debug(const DebugPayload& msg, std::vector<std::uint8_t>& out) {
    const std::size_t length = msg.length < kDebugMaxBytes ? msg.length : kDebugMaxBytes;
    ByteWriter w;
    w.put_u8(static_cast<std::uint8_t>(msg.level));
    w.put_u8(static_cast<std::uint8_t>(length));
    w.put_bytes(msg.text.data(), length);
    out = w.data();
    return true;
}

bool decode_debug(const void* data, std::size_t length, DebugPayload& out) {
    if (length < kDebugHeaderBytes) {
        return false;
    }
    ByteReader r(data, length);
    std::uint8_t level = 0;
    std::uint8_t text_len = 0;
    if (!r.get_u8(level) || !r.get_u8(text_len)) {
        return false;
    }
    if (static_cast<std::size_t>(text_len) > kDebugMaxBytes || r.remaining() < text_len) {
        return false;
    }
    out.level = static_cast<DebugLevel>(level);
    out.length = text_len;
    out.text.fill('\0');
    return r.get_bytes(out.text.data(), text_len);
}

void encode_frame(Frame& frame, std::vector<std::uint8_t>& out) {
    frame.header.magic = kMagic;
    frame.header.version = kProtocolVersion;
    frame.header.payload_len = static_cast<std::uint16_t>(frame.payload.size());
    frame.header.payload_crc =
        frame.payload.empty() ? 0U : crc32(frame.payload.data(), frame.payload.size());

    ByteWriter w;
    w.put_u32(frame.header.magic);
    w.put_u8(frame.header.version);
    w.put_u8(frame.header.stream);
    w.put_u16(frame.header.flags);
    w.put_u32(frame.header.seq);
    w.put_u64(frame.header.timestamp_ns);
    w.put_u16(frame.header.payload_len);
    w.put_u16(frame.header.reserved);
    w.put_u32(frame.header.payload_crc);
    w.put_bytes(frame.payload.data(), frame.payload.size());
    out = w.data();
}

bool decode_frame(const void* data, std::size_t length, Frame& out, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error != nullptr) {
            *error = msg;
        }
        return false;
    };

    if (length < kHeaderSize) {
        return fail("short frame");
    }

    ByteReader reader(data, length);
    FrameHeader header;
    if (!decode_header(reader, header)) {
        return fail("bad header");
    }
    if (header.magic != kMagic) {
        return fail("bad magic");
    }
    if (header.version != kProtocolVersion) {
        return fail("unsupported version");
    }
    if (header.payload_len != length - kHeaderSize) {
        return fail("payload length mismatch");
    }
    if (header.payload_len > 0) {
        const auto* payload = static_cast<const std::uint8_t*>(data) + kHeaderSize;
        if (crc32(payload, header.payload_len) != header.payload_crc) {
            return fail("payload crc mismatch");
        }
    } else if (header.payload_crc != 0U) {
        return fail("non-zero crc on empty payload");
    }

    out.header = header;
    out.payload.assign(static_cast<const std::uint8_t*>(data) + kHeaderSize,
                       static_cast<const std::uint8_t*>(data) + length);
    return true;
}

std::string stream_name(std::uint8_t stream) {
    switch (static_cast<Stream>(stream)) {
        case Stream::Telemetry:
            return "telemetry";
        case Stream::Debug:
            return "debug";
        case Stream::Control:
            return "control";
        case Stream::TestDown:
            return "test-down";
        case Stream::TestUp:
            return "test-up";
    }
    return "stream-" + std::to_string(stream);
}

}  // namespace oal
