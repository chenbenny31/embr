//
// Created by benny on 3/1/26.
//

#pragma once

#include "transport.hpp"
#include "util/socket_fd.hpp"
#include <memory>
#include <cstdint>

// Binds and listens on given port, returns the listening socket fd
SocketFd tcp_listen(uint16_t port);

// Blocks until an incoming connection arrives on listen_fd, returns a TcpTransport
// TCP_NODEPLAY set, SO_SNDBUF left at OS default
std::unique_ptr<Transport> tcp_accept(int listen_fd);

// Adopts a configured fd into TcpTransport, used by bench runner
// caller is responsible for setting TCP_NODELAY before calling
std::unique_ptr<Transport> tcp_from_fd(SocketFd fd);