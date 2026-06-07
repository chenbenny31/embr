//
// Created by benny on 3/14/26.
//

#pragma once

#include "transport.hpp"
#include "util/socket_fd.hpp"
#include <memory>
#include <string>
#include <unistd.h>

// TCP implementation of Transport
// construct only via factories: tcp_connect, tcp_accept, tcp_from_fd
//
// send_file: sendfile() - page cache -> socket, 0 copy
// recv_file: splice() socket -> pipe -> file, 0 copy
//            pipe lazily init, reused across chunks
// SIGPIPE: sendfile() has no MSG_NOSIGNAL equivalent, set signal(SIGPIPE, SIG_IGN) before transfer
class TcpTransport final : public Transport {
public:
    // TCP_NODELAY: disable Nagle, prevents CHUNK_REQ control msg coalescing
    static constexpr int NODELAY_ON = 1;

    ssize_t send(const uint8_t* buf, size_t len) override;
    ssize_t recv(uint8_t* buf, size_t len) override;
    void send_file(int file_fd, uint64_t offset, size_t len) override;
    void recv_file(int file_fd, uint64_t offset, size_t len) override;

    ~TcpTransport() {
        if (pipe_rd_ >= 0) { ::close(pipe_rd_); }
        if (pipe_wr_ >= 0) { ::close(pipe_wr_); }
    }

private:
    explicit TcpTransport(SocketFd fd) : fd_(std::move(fd)) {}
    SocketFd fd_;

    friend std::unique_ptr<Transport> tcp_connect(const std::string& host, uint16_t port);
    friend std::unique_ptr<Transport> tcp_accept(int listen_fd);
    friend std::unique_ptr<Transport> tcp_from_fd(SocketFd fd);

    int pipe_rd_ = -1;
    int pipe_wr_ = -1;
};