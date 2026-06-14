//
// Created by benny on 3/14/26.
//

#include "tcp_transport.hpp"
#include "util/constants.hpp"
#include "util/exact_io.hpp"
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h> // ::splice, SPLICE_F_MOVE, requires _GNU_SOURCE
#include <iostream>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <stdexcept>

// --- Control plane ---
ssize_t TcpTransport::send(const uint8_t* buf, size_t len) {
    return ::send(fd_.get(), buf, len, MSG_NOSIGNAL);
}

ssize_t TcpTransport::recv(uint8_t* buf, size_t len) {
    return ::recv(fd_.get(), buf, len, 0);
}

// --- Data plane ---
// send_file: sendfile() - page cache -> socket buffer, zero copy
// SIGPIPE must be ignored process-wide (in main.cpp)
// sendfile() has no MSG_NOSIGNAL equivalent; peer reset raises SIGPIPE before EPIPE
void TcpTransport::send_file(int file_fd, uint64_t offset, size_t len) {
    size_t remain = len;
    while (remain > 0) { // send
        off_t off = static_cast<off_t>(offset);
        ssize_t sent = ::sendfile(fd_.get(), file_fd, &off, remain);
        if (sent < 0) {
            if (errno == EINTR) { continue; } // retry on signal interrupt
            throw std::runtime_error(
                std::string("TcpTransport::send_file: sendfile failed: ") +
                std::strerror(errno));
        }
        if (sent == 0) {
            // SIGPIPE ignored, sent == 0 is near-dead but handle defensively
            throw std::runtime_error("TcpTransport::send_file: connection closed");
        }
        offset += static_cast<size_t>(sent);
        remain -= static_cast<size_t>(sent);
    }
}

// recv_file: splice() socket -> pipe -> file, zero user-space copy
// pipe is lazily initialized on first call, reused across chunks
void TcpTransport::recv_file(int file_fd, uint64_t offset, size_t len) {
    // lazy pipe init: one pipe per TcpTransport instance
    if (pipe_rd_ < 0) {
        int pipefd[2];
        if (::pipe(pipefd) < 0) {
            throw std::runtime_error(std::string("TcpTransport::recv_file: pipe failed: ") +
                                     std::strerror(errno));
        }
        pipe_rd_ = pipefd[0];
        pipe_wr_ = pipefd[1];

        const int granted = ::fcntl(pipe_wr_, F_SETPIPE_SZ, static_cast<int>(CHUNK_SIZE));
        // franted is the actual pipe size after kernel rounding and clamping at fs.pipe-max-size
        std::cerr << "[tcp] pipe size granted: " << granted << " bytes\n";
    }

    off_t file_offset = static_cast<off_t>(offset);
    size_t remain = len;

    while (remain > 0) {
        // splice socket -> pipe
        ssize_t spliced = ::splice(fd_.get(), nullptr,
                                   pipe_wr_, nullptr,
                                   remain,
                                   SPLICE_F_MOVE);
        if (spliced == 0) {
            throw std::runtime_error("TcpTransport::recv_file: connection closed");
        }
        if (spliced < 0) {
            if (errno == EINTR) { continue; }
            throw std::runtime_error(std::string("TcpTransport::recv_file: socket->pipe failed: ") +
                                     std::strerror(errno));
        }

        // splice pipe -> file at file_offset
        size_t remain_pipe = static_cast<size_t>(spliced);
        while (remain_pipe > 0) {
            ssize_t written = ::splice(pipe_rd_, nullptr,
                                       file_fd, &file_offset,
                                       remain_pipe,
                                       SPLICE_F_MOVE);
            if (written == 0) {
                throw std::runtime_error(std::string("TcpTransport::recv_file: pipe->file EOF"));
            }
            if (written < 0) {
                if (errno == EINTR) { continue; }
                throw std::runtime_error(std::string("TcpTransport::recv_file: pipe->file failed: ") +
                                         std::strerror(errno));
            }
            remain_pipe -= static_cast<size_t>(written);
        }

        remain -= static_cast<size_t>(spliced);
    }
}