//
// Created by benny on 3/1/26.
//

#pragma once
#include <cstdint>
#include <functional>

class TcpServer {
public:
    using Handler = std::function<void(int client_fd)>;

    explicit TcpServer(uint16_t port);
    ~TcpServer();

    void set_handler(Handler handler);
    void start();
    void stop();

    uint16_t port() const { return port_; }

private:
    int server_fd_ = -1;
    uint16_t port_;
    bool running_ = false;
    Handler handler_;
};