//
// Created by benny on 3/1/26.
//

#pragma once

#include "transport.hpp"
#include "util/socket_fd.hpp"
#include <memory>
#include <cstdint>

// Server binds and listens on given port, returns the listening socket fd
SocketFd tcp_listen(uint16_t port);

// Blocks until an incoming connection arrives on listen_fd, returns a TcpTransport
std::unique_ptr<Transport> tcp_accept(int listen_fd);