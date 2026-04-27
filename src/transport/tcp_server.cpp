//
// Created by benny on 3/1/26.
//

#include "tcp_server.hpp"
#include "tcp_transport.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstring>
#include <stdexcept>
#include <cerrno>

SocketFd tcp_listen(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(std::string("tcp_listen: socket() failed: ") + strerror(errno));
    }

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error(std::string("tcp_listen: bind() failed: ") + strerror(errno));
    }

    if (::listen(fd, SOMAXCONN) < 0) {
        ::close(fd);
        throw std::runtime_error(std::string("tcp_listen: listen() failed: ") + strerror(errno));
    }

    return SocketFd(fd);
}

std::unique_ptr<Transport> tcp_accept(int listen_fd) {
    int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd < 0) {
        throw std::runtime_error(std::string("tcp_accept: accept() failed: ") + strerror(errno));
    }
    return std::unique_ptr<Transport>(new TcpTransport(SocketFd{fd}));
}
