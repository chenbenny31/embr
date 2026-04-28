//
// Created by benny on 3/14/26.
//

#pragma once

#include "transport.hpp"
#include "util/socket_fd.hpp"
#include <memory>
#include <string>

// TCP implementation of Transport
// construct only via factories
//
// send_file: sendfile() system call - page cache -> socket, 0 copy
// recv_file: splice() socket -> pipe -> file, 0 copy
//            pipe lazily init, reused across chunks
class TcpTransport final : public Transport {
public:
    // socket optimization
    static constexpr int SNDBUF_SIZE = 4 * 1024 * 1024; // 4MB send buffer over default 128kB
    static constexpr int RCVBUF_SIZE = 4 * 1024 * 1024; // 4MB recv buffer over default 128KB
    static constexpr int TCP_NODELAY_ON = 1; // disable Nagle, prevent CHUNK_REQ msg coalescing

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

    int pipe_rd_ = -1;
    int pipe_wr_ = -1;
};