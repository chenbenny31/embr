//
// Created by benny on 8/28/26.
//

#include "transport/quic_client.hpp"
#include "transport/quic_server.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <gtest/gtest.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

struct QuicListener {
    int      fd;
    uint16_t port;
};

// quic_listen binds INADDR_ANY:0, kernel assigns — same trick as make_listen_socket
static QuicListener make_quic_listener() {
    int fd = quic_listen(0);

    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        ::close(fd);
        throw std::runtime_error("getsockname() failed");
    }
    return {fd, ntohs(addr.sin_port)};
}

struct TempCert {
    std::string cert_path;
    std::string key_path;

    TempCert(std::string cert_path, std::string key_path)
        : cert_path(std::move(cert_path)), key_path(std::move(key_path)) {}

    ~TempCert() {
        if (!cert_path.empty()) { ::unlink(cert_path.c_str()); }
        if (!key_path.empty())  { ::unlink(key_path.c_str()); }
    }
    // move-only
    TempCert(const TempCert&) = delete;
    TempCert& operator=(const TempCert&) = delete;
    TempCert(TempCert&& other) noexcept
        : cert_path(std::move(other.cert_path)),
          key_path(std::move(other.key_path)) {}
};

// self-signed, both endpoints ours — client runs SSL_VERIFY_NONE
static TempCert make_temp_cert() {
    char cert_tmpl[] = "/tmp/embr_quic_cert_XXXXXX";
    char key_tmpl[]  = "/tmp/embr_quic_key_XXXXXX";

    int cert_fd = ::mkstemp(cert_tmpl);
    int key_fd  = ::mkstemp(key_tmpl);
    if (cert_fd < 0 || key_fd < 0) { throw std::runtime_error("mkstemp() failed"); }
    ::close(cert_fd);
    ::close(key_fd);

    const std::string cmd =
        "openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost"
        " -keyout " + std::string{key_tmpl} +
        " -out "    + std::string{cert_tmpl} + " 2>/dev/null";

    if (std::system(cmd.c_str()) != 0) {
        ::unlink(cert_tmpl);
        ::unlink(key_tmpl);
        throw std::runtime_error("openssl req failed");
    }
    return {std::string{cert_tmpl}, std::string{key_tmpl}};
}

// ── Handshake_Completes ───────────────────────────────────────────────────────

TEST(QuicTransport, Handshake_Completes) {
    auto cert = make_temp_cert();
    auto [listen_fd, port] = make_quic_listener();

    // quic_accept throwing inside a jthread would terminate, capture instead
    std::string                server_err;
    std::unique_ptr<Transport> server_side;

    std::jthread server([listen_fd, &cert, &server_err, &server_side]() {
        try {
            server_side = quic_accept(listen_fd, cert.cert_path, cert.key_path);
        } catch (const std::exception& e) {
            server_err = e.what();
        }
    });

    // socket is bound before the thread starts, so the Initial is buffered
    auto client = quic_connect("127.0.0.1", port);
    server.join();

    EXPECT_TRUE(server_err.empty()) << server_err;
    EXPECT_NE(client, nullptr);
    EXPECT_NE(server_side, nullptr);
}

// ── Accept_RejectsMalformedInitial ────────────────────────────────────────────

TEST(QuicTransport, Accept_RejectsMalformedInitial) {
    auto cert = make_temp_cert();
    auto [listen_fd, port] = make_quic_listener();

    std::string server_err;
    std::jthread server([listen_fd, &cert, &server_err]() {
        try {
            auto t = quic_accept(listen_fd, cert.cert_path, cert.key_path);
        } catch (const std::exception& e) {
            server_err = e.what();
        }
    });

    // zeroed datagram: short-header form, never a valid Initial
    int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_GE(fd, 0);

    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port        = htons(port);

    const uint8_t junk[64] = {};
    ::sendto(fd, junk, sizeof(junk), 0,
             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    ::close(fd);

    server.join();
    EXPECT_FALSE(server_err.empty());
}