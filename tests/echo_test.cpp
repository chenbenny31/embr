//
// Created by benny on 3/1/26.
//

#include "transport/tcp_server.hpp"
#include "transport/tcp_client.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>

constexpr uint16_t TEST_PORT = 9000;
constexpr size_t BUF_SIZE = 1024;

TEST(EchoTest, SendAndRecv) {
    TcpServer server(TEST_PORT);

    // run server in background thread
    std::thread server_thread([&server]() {server.start(); });
    // give server time for bind()
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TcpClient client("127.0.0.1", TEST_PORT);
    client.connect();

    const char* msg = "hello embr";
    client.send(msg, strlen(msg));

    char buf[BUF_SIZE]{};
    ssize_t n = client.recv(buf, BUF_SIZE);

    EXPECT_EQ(n, static_cast<ssize_t>(strlen(msg)));
    EXPECT_EQ(std::string(buf, n), "hello embr");

    client.close();
    server.stop();
    server_thread.join();
}