#include "oal/udp_socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace oal {

UdpSocket::UdpSocket(const std::string& bind_addr, std::uint16_t port) {
    std::string error;
    if (bind(bind_addr, port, &error)) {
        return;
    }
    last_error_ = error;
}

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : fd_(other.fd_), last_error_(std::move(other.last_error_)) {
    other.fd_ = -1;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        last_error_ = std::move(other.last_error_);
        other.fd_ = -1;
    }
    return *this;
}

bool UdpSocket::open(std::string* error) {
    if (fd_ >= 0) {
        return true;
    }
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        if (error != nullptr) {
            *error = std::string("socket(): ") + std::strerror(errno);
        }
        return false;
    }
    return true;
}

bool UdpSocket::bind(const std::string& address, std::uint16_t port, std::string* error) {
    if (!open(error)) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (address.empty() || address == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        if (error != nullptr) {
            *error = "invalid bind address: " + address;
        }
        return false;
    }

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (error != nullptr) {
            *error = std::string("bind(") + address + ":" + std::to_string(port) +
                     "): " + std::strerror(errno);
        }
        return false;
    }
    return true;
}

bool UdpSocket::connect(const std::string& address, std::uint16_t port, std::string* error) {
    if (!open(error)) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        if (error != nullptr) {
            *error = "invalid destination address: " + address;
        }
        return false;
    }

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (error != nullptr) {
            *error = std::string("connect(") + address + ":" + std::to_string(port) +
                     "): " + std::strerror(errno);
        }
        return false;
    }
    return true;
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

namespace {

template <typename T>
int setsockopt_int(int fd, int level, int option, T value) {
    return ::setsockopt(fd, level, option, &value, sizeof(value));
}

}  // namespace

bool UdpSocket::set_send_buffer(int bytes) {
    return fd_ >= 0 && setsockopt_int(fd_, SOL_SOCKET, SO_SNDBUF, bytes) == 0;
}

bool UdpSocket::set_recv_buffer(int bytes) {
    return fd_ >= 0 && setsockopt_int(fd_, SOL_SOCKET, SO_RCVBUF, bytes) == 0;
}

bool UdpSocket::set_broadcast(bool enabled) {
    return fd_ >= 0 && setsockopt_int(fd_, SOL_SOCKET, SO_BROADCAST, enabled ? 1 : 0) == 0;
}

bool UdpSocket::set_reuse_address(bool enabled) {
    return fd_ >= 0 && setsockopt_int(fd_, SOL_SOCKET, SO_REUSEADDR, enabled ? 1 : 0) == 0;
}

bool UdpSocket::set_nonblocking(bool enabled) {
    if (fd_ < 0) {
        return false;
    }
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    const int new_flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd_, F_SETFL, new_flags) == 0;
}

std::int64_t UdpSocket::send(const void* data, std::size_t length) const {
    if (fd_ < 0) {
        last_error_ = "socket not open";
        return -1;
    }
    const ssize_t sent = ::send(fd_, data, length, 0);
    if (sent < 0) {
        last_error_ = std::string("send(): ") + std::strerror(errno);
    }
    return sent;
}

std::int64_t UdpSocket::recv(void* data, std::size_t length, int timeout_ms) const {
    if (fd_ < 0) {
        last_error_ = "socket not open";
        return -1;
    }

    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;

    const int ready = ::poll(&pfd, 1, timeout_ms);
    if (ready == 0) {
        return 0;
    }
    if (ready < 0) {
        if (errno == EINTR) {
            return 0;
        }
        last_error_ = std::string("poll(): ") + std::strerror(errno);
        return -1;
    }

    const ssize_t received = ::recv(fd_, data, length, 0);
    if (received < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        last_error_ = std::string("recv(): ") + std::strerror(errno);
        return -1;
    }
    return received;
}

void UdpSocket::clear_pending_error() const {
    if (fd_ < 0) {
        return;
    }
    char byte = 0;
    ::recv(fd_, &byte, sizeof(byte), MSG_DONTWAIT);
}

std::uint16_t UdpSocket::local_port() const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (fd_ < 0 || ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

std::string UdpSocket::local_address() const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (fd_ < 0 || ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return {};
    }
    char buffer[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &addr.sin_addr, buffer, sizeof(buffer));
    return buffer;
}

}  // namespace oal
