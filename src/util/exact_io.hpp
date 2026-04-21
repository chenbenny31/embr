//
// Created by benny on 3/29/26.
//

#pragma once
#include "transport/transport.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <stdexcept>

// I/O utilities
// send & recv exact: loop until exactly len bytes transferred or throw error
inline void send_exact(Transport& t, const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = t.send(buf + sent, len - sent);
        if (n == 0) {
            throw std::runtime_error("send_exact: connection closed");
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { continue; }
            throw std::runtime_error(
                std::string("send_exact: send failed: ") +
                std::strerror(errno));
        }
        sent += static_cast<size_t>(n);
    }
}

inline void recv_exact(Transport& t, uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = t.recv(buf + got, len - got);
        if (n == 0) {
            throw std::runtime_error("recv_exact: connection closed");
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { continue; }
            throw std::runtime_error(
                std::string("recv_exact: recv failed: ") +
                std::strerror(errno));
        }
        got += static_cast<size_t>(n);
    }
}

// Write exactly len bytes to a raw file descriptor
inline void fd_write_exact(int fd, const uint8_t* buf, size_t len) {
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
inline void fd_read_exact(int fd, uint8_t* buf, size_t len) {
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