//
// Created by benny on 3/14/26.
//

#pragma once

#include <memory>

#include "transport.hpp"
#include "../util/socket_fd.hpp"

// Raw TCP send/recv implementation of Transport
class TcpTransport final : public Transport {
public:
    ssize_t send(const uint8_t* buf, size_t len) override;
    ssize_t recv(uint8_t* buf, size_t len) override;

private:
    explicit TcpTransport(SocketFd fd) : fd_(std::move(fd)) {}
    SocketFd fd_;

    friend std::unique_ptr<Transport> tcp_connect(const std::string& host, uint16_t port);
    friend std::unique_ptr<Transport> tcp_accept(int listen_fd);
};
