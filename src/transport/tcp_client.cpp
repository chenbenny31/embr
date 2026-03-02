//
// Created by benny on 3/1/26.
//

#include "tcp_client.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdexcept>

TcpClient::TcpClient(const std::string& host, uint16_t port)
    : host_(host), port_(port) {}

TcpClient::~TcpClient() { close(); }

void TcpClient::connect() {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        throw std::runtime_error("failed to create socket");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);

    // convert human-readable IP string into binary for kernel
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        throw std::runtime_error("failed to parse host address: " + host_);
    }

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
       ::close(fd_); // call global scope using `::<func-name>`
        fd_ = -1;
        throw std::runtime_error("failed to connect to server");
    }
}

ssize_t TcpClient::send(const void* data, size_t len) {
    return ::send(fd_, static_cast<const char*>(data), len, 0);
}

ssize_t TcpClient::recv(void* buf, size_t len) {
    return ::recv(fd_, static_cast<char*>(buf), len, 0);
}

void TcpClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}
