//
// Created by benny on 3/14/26.
//

#include "tcp_transport.hpp"
#include "util/constants.hpp"
#include "util/exact_io.hpp"
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

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
        // set pipe buffer to CHUNK_SIZE: matches one chunk exactly
        ::fcntl(pipe_wr_, F_SETPIPE_SZ, static_cast<int>(CHUNK_SIZE));
    }

    off_t file_offset = static_cast<off_t>(offset);
    size_t remaining = len;

    while (remaining > 0) {
        // splice socket -> pipe
        ssize_t spliced = ::splice(fd_.get(), nullptr,
                                   pipe_wr_, nullptr,
                                   remaining,
                                   SPLICE_F_MOVE | SPLICE_F_MORE);
        if (spliced == 0) {
            throw std::runtime_error("TcpTransport::recv_file: connection closed");
        }
        if (spliced < 0) {
            if (errno == EINTR) { continue; }
            throw std::runtime_error(std::string("TcpTransport::recv_file: socket->pipe failed: ") +
                                     std::strerror(errno));
        }

        // splice pipe -> file at file_offset
        size_t remaining_pipe = static_cast<size_t>(spliced);
        while (remaining_pipe > 0) {
            ssize_t written = ::splice(pipe_rd_, nullptr,
                                       file_fd, &file_offset,
                                       remaining_pipe,
                                       SPLICE_F_MOVE | SPLICE_F_MORE);
            if (written == 0) {
                throw std::runtime_error(std::string("TcpTransport::recv_file: pipe->file EOF"));
            }
            if (written < 0) {
                if (errno == EINTR) { continue; }
                throw std::runtime_error(std::string("TcpTransport::recv_file: pipe->file failed: ") +
                                         std::strerror(errno));
            }
            remaining_pipe -= static_cast<size_t>(written);
        }

        remaining -= static_cast<size_t>(spliced);
    }
}