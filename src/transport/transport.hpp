//
// Created by benny on 3/12/26.
//

#pragma once
#include <sys/types.h>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

// Abstract transport interface
// Low-level byte I/O (control plane):
//   send/recv - used by send_msg/recv_msg only, returns bytes transferred
//               partial transfer, best-efforts
//
// High-level file I/O (data plane):
//   send_file/recv_file - throw error if failed, chunk-level calls
//   each transport implements its own zero-copy strategy
// override per transport for zero-copy:
//    TcpTransport::send_file - sendfile() system call (v0.3, 0 copy push)
//    TcpTransport::recv_file - mmap output + recv_exact (v0.3, 1 copy pull)
//    UdpTransport::send_file - io_uring SEND_FIXED() (v0.4, 0 copy push)
//    UdpTransport::recv_file - io_uring RECV_FIXED() (v0.4, 0 copy pull)
class Transport {
public:
    // --- Control Plane ---
    // Returns bytes transferred, -1 on error, 0 on close
    virtual ssize_t send(const uint8_t* buf, size_t len) = 0;
    virtual ssize_t recv(uint8_t* buf, size_t len) = 0;

    // --- Data Plane ---
    // send_file: reads from file_fd at offset, sends exactly len bytes
    // recv_file: recvs exactly len bytes, writes to file_fd at offset
    //            mmap output file (MAP_SHARED) for direct page cache writing
    virtual void send_file(int file_fd, uint64_t offset, size_t len) = 0;
    virtual void recv_file(int file_fd, uint64_t offset, size_t len) = 0;

    virtual ~Transport() = default;
};

// Exact I/O over Transport

inline void send_exact(Transport& t, const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = t.send(buf + sent, len - sent);
        if (n == 0) {
            throw std::runtime_error("send exact: connection closed");
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { continue; }
            throw std::runtime_error("send exact: send failed: " +
                                     std::string(std::strerror(errno)));
        }
        sent += static_cast<size_t>(n);
    }
}

inline void recv_exact(Transport& t, uint8_t* buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        ssize_t n = t.recv(buf + received, len - received);
        if (n == 0) {
            throw std::runtime_error("recv exact: connection closed");
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { continue; }
            throw std::runtime_error("recv exact: recv failed: " +
                                     std::string(std::strerror(errno)));
        }
        received += static_cast<size_t>(n);
    }
}