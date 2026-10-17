#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace oal {

class UdpSocket {
public:
    UdpSocket() = default;
    UdpSocket(const std::string& bind_addr, std::uint16_t port);
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    bool open(std::string* error = nullptr);
    bool bind(const std::string& address, std::uint16_t port, std::string* error = nullptr);
    bool connect(const std::string& address, std::uint16_t port, std::string* error = nullptr);

    void close();

    bool set_send_buffer(int bytes);
    bool set_recv_buffer(int bytes);
    bool set_broadcast(bool enabled);
    bool set_nonblocking(bool enabled);
    bool set_reuse_address(bool enabled);

    std::int64_t send(const void* data, std::size_t length) const;
    std::int64_t recv(void* data, std::size_t length, int timeout_ms) const;
    void clear_pending_error() const;

    bool valid() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    std::uint16_t local_port() const;
    std::string local_address() const;
    const std::string& last_error() const { return last_error_; }

private:
    int fd_ = -1;
    mutable std::string last_error_;
};

}  // namespace oal
