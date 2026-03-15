//
// Created by benny on 3/14/26.
//

#include "tcp_transport.hpp"
#include <sys/socket.h>
#include <utility>

ssize_t TcpTransport::send(const uint8_t* buf, size_t len) {
    return ::send(fd_.get(), buf, len, MSG_NOSIGNAL);
}

ssize_t TcpTransport::recv(uint8_t* buf, size_t len) {
    return ::recv(fd_.get(), buf, len, 0);
}