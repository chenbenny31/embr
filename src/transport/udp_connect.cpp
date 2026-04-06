//
// Created by benny on 4/5/26.
//

#include "udp_connect.hpp"

#include "util/constants.hpp"
#include "util/socket_fd.hpp"
#include "udp_transport.hpp"
#include "core/protocol.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace {

// HELLO wire format (5 bytes): [UDP_HELLO:u32 BE][PROTOCOL_VERSION:u8]
constexpr size_t HELLO_SIZE = sizeof(uint32_t) + sizeof(uint8_t);

static void send_hello(int fd) {
    uint8_t hello[HELLO_SIZE];
    uint32_t magic_be = htonl(UDP_HELLO);
    std::memcpy(hello, &magic_be, sizeof(magic_be));
    std::memcpy(hello + sizeof(magic_be), &PROTOCOL_VERSION, sizeof(PROTOCOL_VERSION));
    if (::send(fd, hello, sizeof(hello), 0) != static_cast<ssize_t>(HELLO_SIZE)) {
        throw std::runtime_error("udp_connect: failed to send hello: " +
                                 std::string(std::strerror(errno)));
    }
}

static bool recv_hello(int fd, int timeout_ms) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, timeout_ms) <= 0) { return false; }

    uint8_t hello[HELLO_SIZE];
    ssize_t n = ::recv(fd, hello, sizeof(hello), 0);
    if (n != static_cast<ssize_t>(HELLO_SIZE)) { return false; }

    uint32_t magic_be{};
    std::memcpy(&magic_be, hello, sizeof(magic_be));
    if (ntohl(magic_be) != UDP_HELLO) { return false; }
    if (hello[sizeof(magic_be)] != PROTOCOL_VERSION) { return false; }
    return true;
}

} // namespace

std::unique_ptr<Transport> udp_connect(const std::string& host, uint16_t port) {
    // create UDP socket
    int raw_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (raw_fd < 0) {
        throw std::runtime_error("udp_connect: socket() failed: " +
                                 std::string(std::strerror(errno)));
    }
    SocketFd fd{raw_fd};

    // connect - locks peer addr
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr.s_addr) != 1) {
        throw std::runtime_error("udp_connect: invalid address: " + host);
    }
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error("udp_connect: connect() failed: " +
                                 std::string(std::strerror(errno)));
    }

    // HELLO handshake, retry i]up to UDP_HELLO_MAX_RETRIES
    constexpr int HELLO_ATTEMPT_TIMEOUT_MS = 3000;
    for (int attempt = 0; attempt < UDP_HELLO_MAX_RETRIES; ++attempt) {
        send_hello(fd.get());
        if (recv_hello(fd.get(), HELLO_ATTEMPT_TIMEOUT_MS)) {
            // HELLO echoed back - peer locked, ready for transport
            return std::unique_ptr<Transport>(new UdpTransport(std::move(fd)));
        }
    }
    throw std::runtime_error("udp_connect: no response from peer after " +
                             std::to_string(UDP_HELLO_MAX_RETRIES) + " attempts");

}