//
// Created by benny on 3/1/26.
//

#pragma once

#include "transport.hpp"
#include "../util/socket_fd.hpp"
#include <memory>
#include <cstdint>

// Factory: passive side
SocketFd tcp_listen(uint16_t port);
std::unique_ptr<Transport> tcp_accept(int listen_fd);