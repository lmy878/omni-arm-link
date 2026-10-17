#include "oal/wfb_link.hpp"

#include <cstdlib>

namespace oal {

bool parse_endpoint(const std::string& text, Endpoint& out) {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }
    const std::string host = text.substr(0, colon);
    const std::string port = text.substr(colon + 1);
    if (host.empty() || port.empty()) {
        return false;
    }
    try {
        const long value = std::stol(port);
        if (value <= 0 || value > 65535) {
            return false;
        }
        out.host = host;
        out.port = static_cast<std::uint16_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool WfbTxChannel::open(const Endpoint& endpoint, std::string* error) {
    return open(endpoint.host, endpoint.port, error);
}

bool WfbTxChannel::open(const std::string& host, std::uint16_t port, std::string* error) {
    std::string err;
    if (!socket_.connect(host, port, &err)) {
        last_error_ = err;
        if (error != nullptr) {
            *error = err;
        }
        return false;
    }
    socket_.set_send_buffer(1 << 20);
    scratch_.reserve(kWfbMaxPacketBytes);
    return true;
}

bool WfbTxChannel::send(std::uint8_t stream, std::uint16_t flags, std::uint32_t seq,
                        std::uint64_t timestamp_ns, const void* payload, std::size_t length,
                        std::string* error) {
    if (length > kWfbMaxPacketBytes) {
        last_error_ = "payload exceeds wfb packet size";
        if (error != nullptr) {
            *error = last_error_;
        }
        return false;
    }

    Frame frame;
    frame.header.stream = stream;
    frame.header.flags = flags;
    frame.header.seq = seq;
    frame.header.timestamp_ns = timestamp_ns;
    frame.payload.assign(static_cast<const std::uint8_t*>(payload),
                         static_cast<const std::uint8_t*>(payload) + length);
    encode_frame(frame, scratch_);

    std::int64_t sent = socket_.send(scratch_.data(), scratch_.size());
    if (sent < 0) {
        socket_.clear_pending_error();
        sent = socket_.send(scratch_.data(), scratch_.size());
    }
    if (sent < 0) {
        last_error_ = socket_.last_error();
        if (error != nullptr) {
            *error = last_error_;
        }
        return false;
    }

    ++packets_;
    bytes_ += static_cast<std::uint64_t>(sent);
    return true;
}

bool WfbTxChannel::send(std::uint8_t stream, std::uint16_t flags, std::uint32_t seq,
                        std::uint64_t timestamp_ns, const std::vector<std::uint8_t>& payload,
                        std::string* error) {
    return send(stream, flags, seq, timestamp_ns, payload.data(), payload.size(), error);
}

bool WfbRxChannel::open(std::uint16_t port, const std::string& bind_address, std::string* error) {
    std::string err;
    if (reuse_address_) {
        if (!socket_.open(&err)) {
            last_error_ = err;
            if (error != nullptr) {
                *error = err;
            }
            return false;
        }
        socket_.set_reuse_address(true);
    }
    if (!socket_.bind(bind_address, port, &err)) {
        last_error_ = err;
        if (error != nullptr) {
            *error = err;
        }
        return false;
    }
    socket_.set_recv_buffer(4 << 20);
    buffer_.resize(kWfbRecvBufferBytes);
    return true;
}

int WfbRxChannel::recv(Frame& frame, int timeout_ms, bool* crc_error) {
    if (crc_error != nullptr) {
        *crc_error = false;
    }

    const std::int64_t received = socket_.recv(buffer_.data(), buffer_.size(), timeout_ms);
    if (received == 0) {
        return 0;
    }
    if (received < 0) {
        last_error_ = socket_.last_error();
        return -1;
    }

    std::string error;
    if (!decode_frame(buffer_.data(), static_cast<std::size_t>(received), frame, &error)) {
        last_error_ = error;
        if (crc_error != nullptr) {
            *crc_error = true;
        }
        return -1;
    }

    ++packets_;
    bytes_ += static_cast<std::uint64_t>(received);
    return 1;
}

}  // namespace oal
