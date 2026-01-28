#pragma once

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <poll.h>
#include <fcntl.h>
#endif

#include <cerrno>
#include <stdexcept>
#include <concepts>
#include <system_error>
#include <bit>

namespace evqovv {
namespace nova_net {
#if defined(_WIN32)
using socket_t = SOCKET;
using socket_addr_t = sockaddr_in;
using socket_addr_size_t = int;
constexpr socket_t invalid_socket = INVALID_SOCKET;
constexpr auto error_interrupted = WSAEINTR;

class wsa_context {
  public:
    wsa_context() {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) [[unlikely]] {
            throw std::runtime_error("WSAStartup failed.");
        }
    }

    wsa_context(wsa_context const &) = delete;
    wsa_context &operator=(wsa_context const &) = delete;

    ~wsa_context() { ::WSACleanup(); }
};

inline wsa_context wsa;
#else
using socket_t = int;
using socket_addr_t = sockaddr_in;
using socket_addr_size_t = socklen_t;
constexpr socket_t invalid_socket = -1;
constexpr int invalid_epoll = -1;
#endif

namespace detail {
constexpr bool is_little_endian() noexcept {
    return std::endian::native == std::endian::little;
}

template <std::unsigned_integral T> constexpr T byteswap(T value) noexcept {
    T res = 0;
    for (std::size_t i = 0; i < sizeof(T); i++) {
        res <<= 8;
        res |= static_cast<T>(value & 0xFF);
        value >>= 8;
    }
    return value;
}

[[noreturn]] inline void throw_system_exception(const char *what) {
    throw std::system_error(errno, std::generic_category(), what);
}
} // namespace detail

namespace base {
template <std::unsigned_integral T>
constexpr T to_network_byte_order(T value) noexcept {
    if constexpr (detail::is_little_endian()) {
        return detail::byteswap(value);
    } else {
        return value;
    }
}

template <std::unsigned_integral T>
constexpr T to_host_byte_order(T value) noexcept {
    if constexpr (detail::is_little_endian()) {
        return detail::byteswap(value);
    } else {
        return value;
    }
}

inline void set_non_blocking(socket_t fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == invalid_socket) [[unlikely]] {
        detail::throw_system_exception("fcntl failed: ");
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) [[unlikely]] {
        detail::throw_system_exception("fcntl failed: ");
    }
}
inline sockaddr_in to_ipv4_socket_addr(const std::string &ip,
                                       unsigned short port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = to_network_byte_order(port);
    int res = ::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    if (res != 1) [[unlikely]] {
        if (res == 0) [[likely]] {
            throw std::invalid_argument(
                "inet_pton failed: Invalid IPv4 address string.");
        } else [[unlikely]] {
            detail::throw_system_exception("inet_pton failed: ");
        }
    }
    return addr;
}

inline int create_tcp_socket() {
    socket_t fd = ::socket(PF_INET, SOCK_STREAM, 0);
    if (fd == -1) [[unlikely]] {
        detail::throw_system_exception("socket failed: ");
    }
    return fd;
}

inline void enable_address_reuse(socket_t fd) {
    int opt = 1;

    if (::setsockopt(fd, SOL_SOCKET,
#ifdef _WIN32
                     SO_REUSEADDR, reinterpret_cast<const char *>(&opt),
#else
                     SO_REUSEPORT, &opt,
#endif
                     sizeof(opt)) != 0) [[unlikely]] {
        detail::throw_system_exception("setsockopt failed: ");
    }
}

inline void bind(socket_t fd, sockaddr_in addr) {
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        [[unlikely]] {
        detail::throw_system_exception("bind failed: ");
    }
}

inline void listen(socket_t fd) {
    if (::listen(fd, SOMAXCONN) != 0) [[unlikely]] {
        detail::throw_system_exception("listen failed: ");
    }
}

inline socket_t accept(socket_t fd) {
    sockaddr_in addr{};
    socklen_t sz = sizeof(addr);
    socket_t client_fd = ::accept(fd, reinterpret_cast<sockaddr *>(&addr), &sz);
    if (client_fd == invalid_socket) {
        detail::throw_system_exception("accept failed: ");
    }
    return client_fd;
}

inline void close(socket_t fd) noexcept {
#if defined(_WIN32)
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}
} // namespace base
} // namespace nova_net
} // namespace evqovv