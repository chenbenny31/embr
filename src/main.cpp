#include "transport/tcp_server.hpp"
#include "transport/tcp_client.hpp"
#include <iostream>
#include <cstring>

constexpr uint16_t PORT = 9000;
constexpr size_t BUF_SIZE = 1024;

void run_server() {
    TcpServer server(PORT);
    server.start();
}

void run_client() {
    TcpClient client("127.0.0.1", PORT);
    client.connect();

    const char* msg = "hello embr";
    client.send(msg, strlen(msg));

    char buf[BUF_SIZE]{};
    ssize_t n = client.recv(buf, sizeof(buf));
    if (n > 0) {
        std::cout << "[client] echoed: " << std::string(buf, n) << "\n";
    }

    client.close();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: embr <server|client>\n";
        return 1;
    }

    std::string mode = argv[1];
    if (mode == "server") {
        run_server();
    } else if (mode == "client") {
        run_client();
    } else {
        std::cerr << "unknown server mode: " << mode << "\n";
    }

    return 0;
}

