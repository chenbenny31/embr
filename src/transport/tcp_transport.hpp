//
// Created by benny on 3/14/26.
//

#pragma once

#include <memory>
#include <string>
#include "transport.hpp"
#include "../util/socket_fd.hpp"

// TCP implementation of Transport
// construct only via factories
//
// send_file: sendfile() system call - page cache -> socket, 0 copy
// recv_file: mmap output file (MAP_SHARED) + recv_exact
//            recv writes directly into page cache - 1 copy
class TcpTransport final : public Transport {
public:
    ssize_t send(const uint8_t* buf, size_t len) override;
    ssize_t recv(uint8_t* buf, size_t len) override;
    void send_file(int file_fd, uint64_t offset, size_t len) override;
    void recv_file(int file_fd, uint64_t offset, size_t len) override;

private:
    explicit TcpTransport(SocketFd fd) : fd_(std::move(fd)) {}
    SocketFd fd_;

    friend std::unique_ptr<Transport> tcp_connect(const std::string& host, uint16_t port);
    friend std::unique_ptr<Transport> tcp_accept(int listen_fd);
};
