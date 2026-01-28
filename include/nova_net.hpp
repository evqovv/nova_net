#pragma once

#include <functional>
#include <memory>
#include "base.hpp"

namespace evqovv {
namespace nova_net {
class channel {
  public:
    channel(const channel &o) = delete;

    channel &operator=(const channel &o) = delete;

    using handler_t = std::function<void(uint32_t events)>;

    channel(socket_t fd, handler_t handler) noexcept
        : fd_(fd), handler_(std::move(handler)) {}

    void update(handler_t handler) noexcept { handler_ = std::move(handler); }

    void handle(uint32_t events) {
        if (handler_) [[likely]] {
            handler_(events);
        }
    }

    socket_t fd() noexcept { return fd_; }

  private:
    socket_t fd_;
    handler_t handler_;
};

class event_loop {
  public:
    event_loop(const event_loop &o) = delete;

    event_loop &operator=(const event_loop &o) = delete;

    event_loop(size_t max_events = 1024) : events_(max_events) {
        epfd_ = ::epoll_create1(0);
        if (epfd_ == invalid_epoll) [[unlikely]] {
            detail::throw_system_exception("epoll_create1 failed: ");
        }
    }

    ~event_loop() {
        if (epfd_ != invalid_epoll) [[likely]] {
            ::close(epfd_);
        }
    }

    void add_channel(socket_t fd, uint32_t events, channel::handler_t handler) {
        auto ch = std::make_unique<channel>(fd, std::move(handler));
        epoll_event ev{};
        ev.events = events | EPOLLET;
        ev.data.ptr = ch.get();
        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, ch->fd(), &ev) == -1)
            [[unlikely]] {
            detail::throw_system_exception("epoll_ctl failed: ");
        }
        channels_.emplace(fd, std::move(ch));
    }

    void remove_channel(socket_t fd) {
        if (::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) == -1) [[unlikely]] {
            detail::throw_system_exception("epoll_ctl failed: ");
        }
        channels_.erase(fd);
    }

    void update_channel(socket_t fd, uint32_t new_events,
                        channel::handler_t handler) {
        auto ch = channels_.at(fd).get();
        ch->update(std::move(handler));
        epoll_event ev{};
        ev.events = new_events | EPOLLET;
        ev.data.ptr = ch;
        if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == -1) [[unlikely]] {
            detail::throw_system_exception("epoll_ctl failed: ");
        }
    }

    void loop() {
        while (true) {
            auto n = ::epoll_wait(epfd_, events_.data(), events_.size(), -1);
            if (n == -1) [[unlikely]] {
                detail::throw_system_exception("epoll_wait failed: ");
            }
            for (int i = 0; i < n; ++i) {
                auto ch = static_cast<channel *>(events_[i].data.ptr);
                if (ch) [[likely]] {
                    ch->handle(events_[i].events);
                }
            }
        }
    }

  private:
    int epfd_;
    std::vector<epoll_event> events_;
    std::unordered_map<socket_t, std::unique_ptr<channel>> channels_;
};

class tcp_connection {
  public:
    using new_message_handler_t = std::function<void(const std::string &str)>;

    tcp_connection(const tcp_connection &o) = delete;

    tcp_connection &operator=(const tcp_connection &o) = delete;

    tcp_connection(tcp_connection &&o) noexcept : loop_(o.loop_), fd_(o.fd_) {}

    tcp_connection &operator=(tcp_connection &&o) = delete;

    tcp_connection(event_loop &loop, socket_t fd) : loop_(loop), fd_(fd) {
        base::set_non_blocking(fd);

        loop_.add_channel(fd_, EPOLLIN, [this](uint32_t) { recv(); });
    }

    ~tcp_connection() {
        if (fd_ != invalid_socket) [[likely]] {
            base::close(fd_);
        }
    }

    void send(const std::string &data) {
        if (!is_writing_ && out_buffer_.empty()) {
            auto n = ::write(fd_, data.data(), data.size());
            if (n == data.size()) {
                return;
            } else if (n >= 0) {
                out_buffer_.append(data.data() + n, data.size() - n);
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    out_buffer_ += data;
                } else {
                    detail::throw_system_exception("write failed: ");
                }
            }
        } else {
            out_buffer_ += data;
        }

        register_write_event();
    }

    void register_write_event() {
        if (is_writing_) {
            return;
        }

        is_writing_ = true;

        loop_.update_channel(fd_, EPOLLIN | EPOLLOUT,
                             [this](uint32_t ev) {
                                 if (ev & EPOLLIN) {
                                     recv();
                                 }
                                 if (ev & EPOLLOUT) {
                                     handle_write();
                                 }
                             });
    }

    void handle_write() {
        while (!out_buffer_.empty()) {
            auto n = ::write(fd_, out_buffer_.data(), out_buffer_.size());

            if (n > 0) {
                out_buffer_.erase(0, n);
            } else if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                } else {
                    detail::throw_system_exception("write failed: ");
                }
            }
        }

        unregister_write_event();
    }

    void unregister_write_event() {
        if (!is_writing_) {
            return;
        }

        is_writing_ = false;

        loop_.update_channel(fd_, EPOLLIN, [this](uint32_t) { recv(); });
    }

    void recv() {
        char buf[4096]{};
        int cur_idx = 0;
        while (true) {
            auto n = ::read(fd_, buf + cur_idx, sizeof(buf));
            if (n > 0) {
                cur_idx += n;
            } else if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else {
                    detail::throw_system_exception("read failed: ");
                }
            } else {
                loop_.remove_channel(fd_);
            }
        }
        if (new_message_handler_) [[likely]] {
            new_message_handler_(buf);
        }
    }

  private:
    event_loop &loop_;
    socket_t fd_;
    new_message_handler_t new_message_handler_;
    std::string out_buffer_;
    bool is_writing_ = false;
};

class tcp_acceptor {
  public:
    tcp_acceptor(const tcp_acceptor &o) = delete;

    tcp_acceptor &operator=(const tcp_acceptor &o) = delete;

    using new_connection_handler_t =
        std::function<void(std::shared_ptr<tcp_connection> conn)>;

    tcp_acceptor(event_loop &loop, const std::string &ip, unsigned short port)
        : loop_(loop) {
        fd_ = base::create_tcp_socket();
        base::set_non_blocking(fd_);
        base::bind(fd_, base::to_ipv4_socket_addr(ip, port));
        loop_.add_channel(fd_, EPOLLIN,
                          [this](uint32_t events) { on_readable(); });
    }

    ~tcp_acceptor() {
        if (fd_ != invalid_socket) [[likely]] {
            base::close(fd_);
        }
    }

    void start() { base::listen(fd_); }

    void set_new_connection_handler(new_connection_handler_t handler) noexcept {
        handler_ = std::move(handler);
    }

    void on_readable() {
        while (true) {
            socket_t new_fd = ::accept(fd_, nullptr, nullptr);
            if (new_fd != invalid_socket) [[likely]] {
                if (handler_) [[likely]] {
                    handler_(std::make_shared<tcp_connection>(loop_, new_fd));
                } else [[unlikely]] {
                    throw std::runtime_error("you did not specify a handler.");
                }
            } else [[unlikely]] {
                if (errno == EAGAIN || errno == EWOULDBLOCK) [[likely]] {
                    break;
                } else [[unlikely]] {
                    detail::throw_system_exception("accept failed: ");
                }
            }
        }
    }

  private:
    socket_t fd_;
    event_loop &loop_;
    new_connection_handler_t handler_;
};
} // namespace nova_net
} // namespace evqovv