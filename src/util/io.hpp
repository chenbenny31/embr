//
// Created by benny on 3/29/26.
//

#pragma once
#include <cstddef>
#include <sys/socket.h>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include "../transport/transport.hpp"

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