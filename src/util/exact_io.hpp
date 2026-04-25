//
// Created by benny on 3/29/26.
//

#pragma once
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <stdexcept>

// Write exactly len bytes to a raw file descriptor
inline void write_exact(int fd, const uint8_t* buf, size_t len) {
    size_t offset = 0;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::write(fd, buf + offset, remaining);
        if (n == 0) { continue; }
        if (n < 0) {
            if (errno == EINTR) { continue; } // retry on EINTR
            throw std::runtime_error("fd_write_exact: write failed: " +
                                     std::string(std::strerror(errno)));
        }
        offset += static_cast<size_t>(n);
        remaining -= static_cast<size_t>(n);
    }
}

// Read exactly len bytes from a raw file descriptor
inline void read_exact(int fd, uint8_t* buf, size_t len) {
    size_t offset = 0;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::read(fd, buf + offset, remaining);
        if (n == 0) { throw std::runtime_error("fd_read_exact: unexpected EOF"); }
        if (n < 0) {
            if (errno == EINTR) { continue; }
            throw std::runtime_error("fd_read_exact: read failed: " +
                                     std::string(std::strerror(errno)));
        }
        offset += static_cast<size_t>(n);
        remaining -= static_cast<size_t>(n);
    }
}