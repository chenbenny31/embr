//
// Created by benny on 8/28/26.
//

#include "transport/quic_client.hpp"
#include "transport/quic_server.hpp"
#include "transport/quic_transport.hpp"
#include "transport/quic_egress.hpp"
#include "util/exact_io.hpp"
#include "core/protocol.hpp"
#include "util/constants.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <array>
#include <cstring>
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

static void fill_pattern(uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = static_cast<uint8_t>(i & 0xFF);
    }
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

// ── SendRecv_Echo ─────────────────────────────────────────────────────────────

TEST(QuicTransport, SendRecv_Echo) {
    auto cert = make_temp_cert();
    auto [listen_fd, port] = make_quic_listener();

    constexpr size_t kLen = 1024;
    std::array<uint8_t, kLen> send_buf{};
    std::array<uint8_t, kLen> recv_buf{};
    fill_pattern(send_buf.data(), kLen);

    std::string server_err;

    // server: accept → recv_exact → send_exact (echo)
    std::jthread server([listen_fd, &cert, &server_err]() {
        try {
            auto t = quic_accept(listen_fd, cert.cert_path, cert.key_path);
            std::array<uint8_t, kLen> tmp{};
            recv_exact(*t, tmp.data(), kLen);
            send_exact(*t, tmp.data(), kLen);
        } catch (const std::exception& e) {
            server_err = e.what();
        }
    });

    auto client = quic_connect("127.0.0.1", port);
    send_exact(*client, send_buf.data(), kLen);
    recv_exact(*client, recv_buf.data(), kLen);

    server.join();

    EXPECT_TRUE(server_err.empty()) << server_err;
    EXPECT_EQ(std::memcmp(send_buf.data(), recv_buf.data(), kLen), 0);
}

// ── ProtocolRoundTrip ─────────────────────────────────────────────────────────

TEST(QuicTransport, ProtocolRoundTrip) {
    auto cert = make_temp_cert();
    auto [listen_fd, port] = make_quic_listener();

    std::string server_err;
    std::string server_token;

    // server: accept → recv HANDSHAKE → reply COMPLETE
    std::jthread server([listen_fd, &cert, &server_err, &server_token]() {
        try {
            auto t = quic_accept(listen_fd, cert.cert_path, cert.key_path);
            Message received = recv_msg(*t);
            if (received.type != MsgType::HANDSHAKE) {
                server_err = "server: expected HANDSHAKE";
                return;
            }
            server_token = parse_handshake(received).token;
            send_msg(*t, make_complete());
        } catch (const std::exception& e) {
            server_err = e.what();
        }
    });

    auto client = quic_connect("127.0.0.1", port);
    send_msg(*client, make_handshake(HandshakePayload{"Kf3xQ9mZ"}));
    Message reply = recv_msg(*client);

    server.join();

    EXPECT_TRUE(server_err.empty()) << server_err;
    EXPECT_EQ(server_token, "Kf3xQ9mZ");
    EXPECT_EQ(reply.type, MsgType::COMPLETE);
}

// ── ProtocolSequence ──────────────────────────────────────────────────────────
// four messages back to back — catches framing that only works one at a time

TEST(QuicTransport, ProtocolSequence) {
    auto cert = make_temp_cert();
    auto [listen_fd, port] = make_quic_listener();

    constexpr uint32_t kChunkCount = 3;
    std::array<uint8_t, HASH_SIZE> hash0{}, hash1{}, hash2{};
    hash0.fill(0xAA); hash1.fill(0xBB); hash2.fill(0xCC);

    std::string server_err;
    FileMeta    server_meta{};
    uint32_t    server_req_index = 0;

    // server: FILE_META → CHUNK_REQ, then CHUNK_HDR → COMPLETE
    std::jthread server([listen_fd, &cert, &server_err,
                         &server_meta, &server_req_index]() {
        try {
            auto t = quic_accept(listen_fd, cert.cert_path, cert.key_path);
            server_meta = parse_filemeta(recv_msg(*t));
            send_msg(*t, make_chunk_req(ChunkReq{1}));
            server_req_index = parse_chunk_hdr(recv_msg(*t)).chunk_index;
            send_msg(*t, make_complete());
        } catch (const std::exception& e) {
            server_err = e.what();
        }
    });

    FileMeta meta{
        .file_name    = "testfile.iso",
        .file_size    = 3ULL * 1024 * 1024 * 1024,
        .chunk_size   = static_cast<uint32_t>(CHUNK_SIZE),
        .chunk_count  = kChunkCount,
        .chunk_hashes = {hash0, hash1, hash2},
    };

    auto client = quic_connect("127.0.0.1", port);
    send_msg(*client, make_filemeta(FileMeta{meta}));

    ChunkReq req = parse_chunk_req(recv_msg(*client));
    send_msg(*client, make_chunk_hdr(ChunkHdr{.chunk_index = req.chunk_index}));
    Message done = recv_msg(*client);

    server.join();

    EXPECT_TRUE(server_err.empty()) << server_err;
    EXPECT_EQ(server_meta.file_name,   meta.file_name);
    EXPECT_EQ(server_meta.file_size,   meta.file_size);
    EXPECT_EQ(server_meta.chunk_count, kChunkCount);
    ASSERT_EQ(server_meta.chunk_hashes.size(), kChunkCount);
    EXPECT_EQ(server_meta.chunk_hashes[0], hash0);
    EXPECT_EQ(server_meta.chunk_hashes[2], hash2);
    EXPECT_EQ(req.chunk_index,     1U);
    EXPECT_EQ(server_req_index,    1U);
    EXPECT_EQ(done.type,           MsgType::COMPLETE);
}

// ── SendFin_PeerRecvReturnsZero ───────────────────────────────────────────────

TEST(QuicTransport, SendFin_PeerRecvReturnsZero) {
    auto cert = make_temp_cert();
    auto [listen_fd, port] = make_quic_listener();

    constexpr size_t kLen = 64;
    std::array<uint8_t, kLen> send_buf{};
    fill_pattern(send_buf.data(), kLen);

    std::string server_err;
    ssize_t     eof_result = -99;

    // server: drain the payload, then the next recv() must see peer FIN
    std::jthread server([listen_fd, &cert, &server_err, &eof_result]() {
        try {
            auto t = quic_accept(listen_fd, cert.cert_path, cert.key_path);
            std::array<uint8_t, kLen> tmp{};
            recv_exact(*t, tmp.data(), kLen);
            eof_result = t->recv(tmp.data(), tmp.size());
        } catch (const std::exception& e) {
            server_err = e.what();
        }
    });

    auto client = quic_connect("127.0.0.1", port);
    send_exact(*client, send_buf.data(), kLen);
    ASSERT_EQ(static_cast<QuicTransport*>(client.get())->send_fin(), 0);

    server.join();

    EXPECT_TRUE(server_err.empty()) << server_err;
    EXPECT_EQ(eof_result, 0); // clean EOF, not -1
}

// ── Close_PeerRecvReturnsMinusOne ─────────────────────────────────────────────

TEST(QuicTransport, Close_PeerRecvReturnsMinusOne) {
    auto cert = make_temp_cert();
    auto [listen_fd, port] = make_quic_listener();

    std::string server_err;
    ssize_t     abnormal_result = -99;

    std::promise<void> accepted;
    auto accepted_ready = accepted.get_future();

    // server: no data ever arrives, only the client's CONNECTION_CLOSE
    std::jthread server([listen_fd, &cert, &server_err, &abnormal_result, &accepted]() {
        std::unique_ptr<Transport> t;
        try {
            t = quic_accept(listen_fd, cert.cert_path, cert.key_path);
        } catch (const std::exception& e) {
            server_err = e.what();
        }
        accepted.set_value(); // exactly once, succ or fail
        if (!t) { return; }
        try {
            uint8_t buf[64];
            abnormal_result = t->recv(buf, sizeof(buf));
        } catch (const std::exception& e) {
            server_err = e.what();
        }
    });


    {
        auto client = quic_connect("127.0.0.1", port);
        // client completes ASAP it processes the server's Fin, could precede over client's Fin
        accepted_ready.wait();
    } // destructs: NO_ERROR close

    server.join();

    EXPECT_TRUE(server_err.empty()) << server_err;
    EXPECT_EQ(abnormal_result, -1); // closed without FIN
}

// ── SendFile_RecvFile_Aligned ─────────────────────────────────────────────────

TEST(QuicTransport, SendFile_RecvFile_Aligned) {
    auto cert = make_temp_cert();
    auto [listen_fd, port] = make_quic_listener();

    constexpr size_t kLen = 64 * 1024;

    auto src = make_tempfile();
    std::vector<uint8_t> pattern(kLen);
    fill_pattern(pattern.data(), kLen);
    ::write(src.fd, pattern.data(), kLen);

    auto dst = make_tempfile();
    ::ftruncate(dst.fd, static_cast<off_t>(kLen));

    std::string server_err;

    // server: accept -> recv_file -> 1-byte ack, so the client can retire its blocks
    std::jthread server([listen_fd, &cert, dst_fd = dst.fd, &server_err]() {
        try {
            auto t = quic_accept(listen_fd, cert.cert_path, cert.key_path);
            t->recv_file(dst_fd, 0, kLen);
            const uint8_t ack = 1;
            send_exact(*t, &ack, 1);
        } catch (const std::exception& e) {
            server_err = e.what();
        }
    });

    auto client = quic_connect("127.0.0.1", port);
    client->send_file(src.fd, 0, kLen / 2);
    client->send_file(src.fd, kLen / 2, kLen / 2); // split send to make deque hold multi-entries

    uint8_t ack = 0;
    recv_exact(*client, &ack, 1); // returns only once the server has every byte

    server.join();
    ::close(listen_fd);

    EXPECT_TRUE(server_err.empty()) << server_err;
    EXPECT_EQ(ack, 1);

    // block 1 retires long before block 2 is sent: proves the ack callback fires
    auto* qt = dynamic_cast<QuicTransport*>(client.get());
    ASSERT_NE(qt, nullptr);
    EXPECT_LT(qt->unacked_bytes(), kLen); // unacked_bytes() can be 0, 32768 or 65536

    std::vector<uint8_t> src_buf(kLen), dst_buf(kLen);
    ::pread(src.fd, src_buf.data(), kLen, 0);
    ::pread(dst.fd, dst_buf.data(), kLen, 0);
    EXPECT_EQ(std::memcmp(src_buf.data(), dst_buf.data(), kLen), 0);
}

// ── Egress_ZcPathCarriesTransfer ──────────────────────────────────────────────
// skipped when EMBR_QUIC_EGRESS=sendmsg (the same suite gates both modes)
TEST(QuicTransport, Egress_ZcPathCarriesTransfer) {
    const char* mode = std::getenv("EMBR_QUIC_EGRESS");
    if (mode && std::string(mode) == "sendmsg") { GTEST_SKIP() << "sendmsg mode"; }
    auto cert = make_temp_cert();
    auto [listen_fd, port] = make_quic_listener();
    constexpr size_t kLen = 256 * 1024;

    auto src = make_tempfile();
    std::vector<uint8_t> pattern(kLen);
    fill_pattern(pattern.data(), kLen);
    ::write(src.fd, pattern.data(), kLen);
    auto dst = make_tempfile();
    ::ftruncate(dst.fd, static_cast<off_t>(kLen));

    std::string server_err;
    std::jthread server([listen_fd, &cert, dst_fd = dst.fd, &server_err]() {
        try {
            auto t = quic_accept(listen_fd, cert.cert_path, cert.key_path);
            t->recv_file(dst_fd, 0, kLen);
            const uint8_t ack = 1;
            send_exact(*t, &ack, 1);
        } catch (const std::exception& e) {
            server_err = e.what();
        }
    });

    auto client = quic_connect("127.0.0.1", port);
    client->send_file(src.fd, 0, kLen);
    uint8_t ack = 0;
    recv_exact(*client, &ack, 1);
    server.join();
    ::close(listen_fd);

    EXPECT_TRUE(server_err.empty()) << server_err;
    auto* qt = dynamic_cast<QuicTransport*>(client.get());
    ASSERT_NE(qt, nullptr);
    ASSERT_NE(qt->zc_egress(), nullptr) << "io_uring unavailable: run with EMBR_QUIC_EGRESS=sendmsg";
    const auto& st = qt->zc_egress()->stats();
    EXPECT_GT(st.sends, 0u);
    EXPECT_EQ(st.errors, 0u);
    EXPECT_EQ(qt->zc_egress()->error(), 0);

    std::vector<uint8_t> src_buf(kLen), dst_buf(kLen);
    ::pread(src.fd, src_buf.data(), kLen, 0);
    ::pread(dst.fd, dst_buf.data(), kLen, 0);
    EXPECT_EQ(std::memcmp(src_buf.data(), dst_buf.data(), kLen), 0);
}

// ── SendFile_RecvFile_UnalignedOffset ─────────────────────────────────────────

TEST(QuicTransport, SendFile_RecvFile_UnalignedOffset) {
    auto cert = make_temp_cert();
    auto [listen_fd, port] = make_quic_listener();

    constexpr size_t kOff = 1000;   // not page-aligned: exercises the mmap delta
    constexpr size_t kLen = 65537;  // not a power of two: last-chunk edge

    auto src = make_tempfile();
    std::vector<uint8_t> pattern(kLen);
    fill_pattern(pattern.data(), kLen);
    ::pwrite(src.fd, pattern.data(), kLen, kOff);

    auto dst = make_tempfile();
    ::ftruncate(dst.fd, static_cast<off_t>(kOff + kLen));

    std::string server_err;

    std::jthread server([listen_fd, &cert, dst_fd = dst.fd, &server_err]() {
        try {
            auto t = quic_accept(listen_fd, cert.cert_path, cert.key_path);
            t->recv_file(dst_fd, kOff, kLen);
            const uint8_t ack = 1;
            send_exact(*t, &ack, 1);
        } catch (const std::exception& e) {
            server_err = e.what();
        }
    });

    auto client = quic_connect("127.0.0.1", port);
    client->send_file(src.fd, kOff, kLen);

    uint8_t ack = 0;
    recv_exact(*client, &ack, 1);

    server.join();
    ::close(listen_fd);

    EXPECT_TRUE(server_err.empty()) << server_err;

    std::vector<uint8_t> src_buf(kLen), dst_buf(kLen);
    ::pread(src.fd, src_buf.data(), kLen, kOff);
    ::pread(dst.fd, dst_buf.data(), kLen, kOff);
    EXPECT_EQ(std::memcmp(src_buf.data(), dst_buf.data(), kLen), 0);
}
