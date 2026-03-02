//
// Created by benny on 3/1/26.
//

#include "tcp_server.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <iostream>

constexpr size_t BUF_SIZE = 4096;

TcpServer::TcpServer(uint16_t port) : port_(port) {}

TcpServer::~TcpServer() { stop(); }

void TcpServer::set_handler(Handler handler) {
    handler_ = std::move(handler);
}

void TcpServer::start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ == -1) {
        throw std::runtime_error("failed to create server socket");
    }

    // socket option: SO_REUSEADDR prevent TIME_WAIT of re bind() for 60 seconds
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error("failed to bind server socket");
    }

    if (listen(server_fd_, 8) < 0) {
        throw std::runtime_error("failed to listen on server socket");
    }

    running_ = true;
    std::cout << "[server] listening on port " << port_ << "\n";

    while (running_) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        // reinterpret_case of cpp is equivalent to (struct sockaddr*)*addr in c, sockaddr_in -> sockaddr
        int client_fd = accept(server_fd_, reinterpret_cast<sockaddr*>(&client_addr),&client_len);
        if (client_fd < 0) {
            if (!running_) { break; }
            std::cout << "[server] failed to accept connection from client socket\n";
            continue;
        }

        std::cout << "[server] accepted connection from client socket (fd=" << client_fd << ")\n";

        if (handler_) {
            handler_(client_fd);
        } else {
            // default echo
            char buf[BUF_SIZE];
            while (true) {
                ssize_t n = recv(client_fd, buf, BUF_SIZE, 0);
                if (n <= 0) { break; } // n = 0 dis-connected; n < 0 error
                send(client_fd, buf, n, 0);
            }
        }

        close(client_fd);
        std::cout << "[server] closed connection from client fd=" << client_fd << "\n";
    }
}

void TcpServer::stop() {
    running_ = false;
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
}