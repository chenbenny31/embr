//
// Created by benny on 3/11/26.
//

#pragma once

#include <unistd.h>
#include <stdexcept>
#include <utility>

// RAII wrapper of POSIX file descriptor, move-only
class SocketFd {
public:
    explicit SocketFd(int fd) : fd_(fd) {
        if (fd_ < 0) { throw std::runtime_error("invalid socket fd"); }
    }
    ~SocketFd() noexcept { if (fd_ >= 0) ::close(fd_); }

    // disable copy constructor -- fd is unique
    SocketFd(const SocketFd&) = delete;
    SocketFd& operator=(SocketFd&) = delete;

    // enable move constructor
    SocketFd(SocketFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

    SocketFd& operator=(SocketFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) { ::close(fd_); }
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    int get() const { return fd_; }

private:
    int fd_;
};