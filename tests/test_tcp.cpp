//
// Created by benny on 3/29/26.
//
#include "transport/tcp_client.hpp"
#include "transport/tcp_server.hpp"
#include "util/exact_io.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cstring>
#include <memory>
#include <thread>
#include <gtest/gtest.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

struct ListenSocket {
    int      fd;
    uint16_t port;
};

static ListenSocket make_listen_socket() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { throw std::runtime_error("socket() failed"); }

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;   // let OS pick

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("bind() failed");
    }
    if (::listen(fd, 1) < 0) {
        ::close(fd);
        throw std::runtime_error("listen() failed");
    }

    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    return {fd, ntohs(addr.sin_port)};
}

struct TempFile {
    int         fd;
    std::string path;

    TempFile(int fd, std::string path) : fd(fd), path(std::move(path)) {}

    ~TempFile() {
        if (fd >= 0) { ::close(fd); }
        if (!path.empty()) { ::unlink(path.c_str()); }
    }
    // move-only
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&& other) noexcept
        : fd(other.fd), path(std::move(other.path)) { other.fd = -1; }
};

static TempFile make_tempfile() {
    char tmpl[] = "/tmp/embr_test_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) { throw std::runtime_error("mkstemp() failed"); }
    return {fd, std::string{tmpl}};
}

// Fill a buffer with a deterministic pattern
static void fill_pattern(uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(i & 0xFF);
    }
}

// ── SendRecv_Echo ─────────────────────────────────────────────────────────────

TEST(TcpTransport, SendRecv_Echo) {
    auto [listen_fd, port] = make_listen_socket();

    constexpr size_t kLen = 1024;
    std::array<uint8_t, kLen> send_buf{};
    std::array<uint8_t, kLen> recv_buf{};
    fill_pattern(send_buf.data(), kLen);

    // server: accept → recv_exact → send_exact (echo)
    std::jthread server([listen_fd, &send_buf, kLen]() {
        auto t = tcp_accept(listen_fd);
        std::array<uint8_t, kLen> tmp{};
        recv_exact(*t, tmp.data(), kLen);
        send_exact(*t, tmp.data(), kLen);
    });

    auto client = tcp_connect("127.0.0.1", port);
    send_exact(*client, send_buf.data(), kLen);
    recv_exact(*client, recv_buf.data(), kLen);

    ::close(listen_fd);
    EXPECT_EQ(std::memcmp(send_buf.data(), recv_buf.data(), kLen), 0);
}

// ── SendFile_RecvFile_Aligned ─────────────────────────────────────────────────

TEST(TcpTransport, SendFile_RecvFile_Aligned) {
    auto [listen_fd, port] = make_listen_socket();

    constexpr size_t kLen = 64 * 1024;   // 64KB — aligned

    // write known pattern into source tempfile
    auto src = make_tempfile();
    std::array<uint8_t, kLen> pattern{};
    fill_pattern(pattern.data(), kLen);
    ::write(src.fd, pattern.data(), kLen);

    // dest tempfile — pre-allocate so recv_file can mmap it
    auto dst = make_tempfile();
    ::ftruncate(dst.fd, static_cast<off_t>(kLen));

    // server: accept → recv_file into dst
    std::jthread server([listen_fd, dst_fd = dst.fd, kLen]() {
        auto t = tcp_accept(listen_fd);
        t->recv_file(dst_fd, 0, kLen);
    });

    auto client = tcp_connect("127.0.0.1", port);
    client->send_file(src.fd, 0, kLen);

    server.join();
    ::close(listen_fd);

    // compare src and dst
    std::array<uint8_t, kLen> src_buf{}, dst_buf{};
    ::pread(src.fd, src_buf.data(), kLen, 0);
    ::pread(dst.fd, dst_buf.data(), kLen, 0);
    EXPECT_EQ(std::memcmp(src_buf.data(), dst_buf.data(), kLen), 0);
}

// ── SendFile_RecvFile_Unaligned ───────────────────────────────────────────────

TEST(TcpTransport, SendFile_RecvFile_Unaligned) {
    auto [listen_fd, port] = make_listen_socket();

    constexpr size_t kLen = 65537;   // not a power of two — exercises last-chunk edge

    auto src = make_tempfile();
    std::vector<uint8_t> pattern(kLen);
    fill_pattern(pattern.data(), kLen);
    ::write(src.fd, pattern.data(), kLen);

    auto dst = make_tempfile();
    ::ftruncate(dst.fd, static_cast<off_t>(kLen));

    std::jthread server([listen_fd, dst_fd = dst.fd, kLen]() {
        auto t = tcp_accept(listen_fd);
        t->recv_file(dst_fd, 0, kLen);
    });

    auto client = tcp_connect("127.0.0.1", port);
    client->send_file(src.fd, 0, kLen);

    server.join();
    ::close(listen_fd);

    std::vector<uint8_t> src_buf(kLen), dst_buf(kLen);
    ::pread(src.fd, src_buf.data(), kLen, 0);
    ::pread(dst.fd, dst_buf.data(), kLen, 0);
    EXPECT_EQ(std::memcmp(src_buf.data(), dst_buf.data(), kLen), 0);
}