//
// Created by benny on 3/14/26.
//

#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include "tcp_transport.hpp"
#include "../util/io.hpp"
#include "../util/constants.hpp"

// --- Control plane ---
ssize_t TcpTransport::send(const uint8_t* buf, size_t len) {
    return ::send(fd_.get(), buf, len, MSG_NOSIGNAL);
}

ssize_t TcpTransport::recv(uint8_t* buf, size_t len) {
    return ::recv(fd_.get(), buf, len, 0);
}

// --- Data plane ---

// send_file: sendfile() - page cache -> socket buffer, zero user-space copy
void TcpTransport::send_file(int file_fd, uint64_t offset, size_t len) {
    size_t remain = len;
    while (remain > 0) { // send
        off_t off = static_cast<off_t>(offset);
        ssize_t sent = ::sendfile(fd_.get(), file_fd, &off, remain);
        if (sent <= 0) {
            throw std::runtime_error(
                std::string("TcpTransport::send_file: sendfile failed: ") +
                std::strerror(errno));
        }
        offset += static_cast<size_t>(sent);
        remain -= static_cast<size_t>(sent);
    }
}

// recv_file: mmap out file (MAP_SHARED) + recv_exact
void TcpTransport::recv_file(int file_fd, uint64_t offset, size_t len) {
    size_t remain = len;
    while (remain > 0) {
        size_t bulk = std::min(remain, READ_BUF_SIZE);
        void* mapped = ::mmap(nullptr, bulk, PROT_READ | PROT_WRITE, MAP_SHARED,
                              file_fd, static_cast<off_t>(offset));
        if (mapped == MAP_FAILED) {
            throw std::runtime_error(
                std::string("TcpTransport::recv_file: mmap failed: ") +
                std::strerror(errno));
        }

        // recv directly into mmap region: 1 copy - socket buf -> page cache
        size_t got = 0;
        while (got < bulk) {
            ssize_t n = ::recv(fd_.get(),
                               static_cast<uint8_t*>(mapped) + got,
                               bulk - got, 0);
            if (n == 0) {
                ::munmap(mapped, bulk);
                throw std::runtime_error(
                    "TcpTransport::recv_file: connection closed");
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) { continue; }
                ::munmap(mapped, bulk);
                throw std::runtime_error(
                    std::string("TcpTransport::recv_file: recv failed: ") +
                    std::strerror(errno));
            }
            got += static_cast<size_t>(n);
        }
        ::munmap(mapped, bulk);
        offset += bulk;
        remain-= bulk;
    }
}