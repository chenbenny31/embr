//
// Created by benny on 3/1/26.
//

#pragma once
#include <cstdint>
#include <string>

class TcpClient {
public:
    TcpClient(const std::string& host, uint16_t port);
    ~TcpClient();

    void connect();
    ssize_t send(const void* data, size_t len);
    ssize_t recv(void* buf, size_t len);
    void close();

    bool connected() const { return fd_ >= 0; }

private:
    std::string host_;
    uint16_t port_;
    int fd_ = -1;
};