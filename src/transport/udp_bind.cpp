//
// Created by benny on 4/5/26.
//

#include "udp_bind.hpp"
#include "udp_transport.hpp"

#include "core/protocol.hpp"
#include "util/constants.hpp"
#include "util/socket_fd.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>


namespace {

constexpr size_t HELLO_SIZE = sizeof(uint32_t) + sizeof(uint8_t);

static bool validate_hello(const uint8_t *buf, size_t len) {
    if (len != HELLO_SIZE) { return false; }
    uint32_t magic_be{};
    std::memcpy(&magic_be, buf, sizeof(magic_be));
    if (ntohl(magic_be) != UDP_HELLO) { return false; }
    if (buf[sizeof(magic_be)] != PROTOCOL_VERSION) { return false; }
    return true;
}

static void send_hello(int fd) {
    uint8_t hello[HELLO_SIZE];
    uint32_t magic_be = htonl(UDP_HELLO);
    std::memcpy(hello, &magic_be, sizeof(magic_be));
    std::memcpy(hello + sizeof(magic_be), &PROTOCOL_VERSION, sizeof(PROTOCOL_VERSION));
    if (::send(fd, hello, sizeof(hello), 0) != static_cast<ssize_t>(HELLO_SIZE)) {
        throw std::runtime_error("send_hello: send HELLO echo failed: " +
                                 std::string(std::strerror(errno)));
    }
}

}

std::unique_ptr<Transport> udp_bind(uint16_t port) {
    // create and bind UDP socket
    int raw_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (raw_fd < 0) {
        throw std::runtime_error("udp_bind: socket() failed: " +
                                 std::string(std::strerror(errno)));
    }
    SocketFd fd{raw_fd};

    int opt = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        throw std::runtime_error("udp_bind: bind() failed: " +
                                 std::string(std::strerror(errno)));
    }

    // wait for HELLO to know peer addr
    pollfd pfd{};
    pfd.fd = fd.get();
    pfd.events = POLLIN;

    int ret = ::poll(&pfd, 1, UDP_HELLO_TIMEOUT_S * 1000);
    if (ret <= 0) {
        throw std::runtime_error("udp_bind: no peer connected within " +
                                 std::to_string(UDP_HELLO_TIMEOUT_S) + "s");
    }

    uint8_t hello_buf[HELLO_SIZE];
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    ssize_t n = ::recvfrom(fd.get(), hello_buf, sizeof(hello_buf), 0,
                           reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (!validate_hello(hello_buf, static_cast<size_t>(n))) {
        throw std::runtime_error("udp_bind: invalid HELLO from peer");
    }

    // lock peer via connect()
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
        throw std::runtime_error("udp_bind: connect() to peer failed: " +
                                 std::string(std::strerror(errno)));
    }

    // echo HELLO back
    send_hello(fd.get());

    return std::unique_ptr<Transport>(new UdpTransport(std::move(fd)));
}
