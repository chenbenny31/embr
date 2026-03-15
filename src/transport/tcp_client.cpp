//
// Created by benny on 3/1/26.
//

#include "tcp_client.hpp"
#include "tcp_transport.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <stdexcept>

std::unique_ptr<Transport> tcp_connect(const std::string& host, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(std::string("tcp_connect: socket() failed: " ) + strerror(errno));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr.s_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("tcp_connect: invalid address: " + host);
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error(std::string("tcp_connect: connect() failed: ") + strerror(errno));
    }

    return std::unique_ptr<Transport>(new TcpTransport(SocketFd{fd}));
}