#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "oal/codec.hpp"
#include "oal/protocol.hpp"
#include "oal/udp_socket.hpp"

namespace oal {

constexpr std::size_t kWfbMaxPacketBytes = 3993;
constexpr std::size_t kWfbRecvBufferBytes = 4096;

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
};

bool parse_endpoint(const std::string& text, Endpoint& out);

class WfbTxChannel {
public:
    WfbTxChannel() = default;

    bool open(const Endpoint& endpoint, std::string* error = nullptr);
    bool open(const std::string& host, std::uint16_t port, std::string* error = nullptr);

    bool send(std::uint8_t stream, std::uint16_t flags, std::uint32_t seq,
              std::uint64_t timestamp_ns, const void* payload, std::size_t length,
              std::string* error = nullptr);

    bool send(std::uint8_t stream, std::uint16_t flags, std::uint32_t seq,
              std::uint64_t timestamp_ns, const std::vector<std::uint8_t>& payload,
              std::string* error = nullptr);

    std::uint64_t packets() const { return packets_; }
    std::uint64_t bytes() const { return bytes_; }
    const std::string& last_error() const { return last_error_; }

private:
    UdpSocket socket_;
    std::vector<std::uint8_t> scratch_;
    std::uint64_t packets_ = 0;
    std::uint64_t bytes_ = 0;
    std::string last_error_;
};

class WfbRxChannel {
public:
    WfbRxChannel() = default;

    void set_reuse_address(bool enabled) { reuse_address_ = enabled; }

    bool open(std::uint16_t port, const std::string& bind_address = "0.0.0.0",
              std::string* error = nullptr);

    int recv(Frame& frame, int timeout_ms, bool* crc_error = nullptr);

    void set_recv_buffer(int bytes) { socket_.set_recv_buffer(bytes); }

    int fd() const { return socket_.fd(); }
    std::uint64_t packets() const { return packets_; }
    std::uint64_t bytes() const { return bytes_; }
    const std::string& last_error() const { return last_error_; }

private:
    UdpSocket socket_;
    std::vector<std::uint8_t> buffer_;
    std::uint64_t packets_ = 0;
    std::uint64_t bytes_ = 0;
    bool reuse_address_ = false;
    std::string last_error_;
};

}  // namespace oal
