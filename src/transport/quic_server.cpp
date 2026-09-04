//
// Created by benny on 8/27/26.
//

#include "quic_server.hpp"
#include "quic_transport.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

namespace {

static uint64_t timestamp_ns() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

static void make_cid(ngtcp2_cid* cid, size_t len) {
    cid->datalen = len;
    (void)::getrandom(cid->data, len, 0);
}

// ngtcp2 reports discarded packets and handshake faults only through this sink
static void log_printf(void*, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n > 0) {
        const size_t len = std::min(static_cast<size_t>(n), sizeof(buf) - 2);
        buf[len] = '\n';
        std::fwrite(buf, 1, len + 1, stderr);
    }
}

} // namespace

int quic_listen(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        throw std::runtime_error(
            std::string("quic_listen: socket() failed: ") + std::strerror(errno));
    }

    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    const int sock_buf = 4 * 1024 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sock_buf, sizeof(sock_buf));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sock_buf, sizeof(sock_buf));

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error(
            std::string("quic_listen: bind() failed: ") + std::strerror(errno));
    }

    return fd;
}

std::unique_ptr<Transport> quic_accept(int listen_fd,
                                       const std::string& cert_path,
                                       const std::string& key_path) {
    // blocking read of the client's Initial: gives the peer addr for connect() and CIDs
    // listen_fd must be blocking on entry
    uint8_t first_pkt[QUIC_MAX_RECV_PKTLEN];
    sockaddr_storage peer_addr{};

    struct iovec iov{};
    iov.iov_base = first_pkt;
    iov.iov_len = sizeof(first_pkt);

    struct msghdr msg{};
    msg.msg_name = &peer_addr;
    msg.msg_namelen = sizeof(peer_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    const ssize_t nread = ::recvmsg(listen_fd, &msg, 0);
    if (nread < 0) {
        ::close(listen_fd);
        throw std::runtime_error(
            std::string("quic_accept: recvmsg() failed: ") + std::strerror(errno));
    }
    if (msg.msg_flags & MSG_TRUNC) {
        ::close(listen_fd);
        throw std::runtime_error("quic_accept: first packet truncated");
    }
    const socklen_t peer_len = msg.msg_namelen;

    ngtcp2_version_cid vc{};
    int rv = ngtcp2_pkt_decode_version_cid(&vc, first_pkt,
                                           static_cast<size_t>(nread),
                                           NGTCP2_MAX_CIDLEN);
    if (rv != 0) {
        ::close(listen_fd);
        throw std::runtime_error(
            std::string("quic_accept: decode_version_cid() failed: ") + ngtcp2_strerror(rv));
    }

    // rejects anything that isn't a well-formed Initial before allocating conn
    ngtcp2_pkt_hd hd{};
    rv = ngtcp2_accept(&hd, first_pkt, static_cast<size_t>(nread));
    if (rv != 0) {
        ::close(listen_fd);
        throw std::runtime_error(
            std::string("quic_accept: not a valid Initial: ") + ngtcp2_strerror(rv));
    }

    // connect assigns the local addr the path depends on and filters later datagrams to peer
    if (::connect(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), peer_len) < 0) {
        ::close(listen_fd);
        throw std::runtime_error(
            std::string("quic_accept: connect() failed: ") + std::strerror(errno));
    }

    // non-blocking, pump_once() can select()
    if (::fcntl(listen_fd, F_SETFL, O_NONBLOCK) < 0) {
        ::close(listen_fd);
        throw std::runtime_error(
            std::string("quic_accept: fcntl(O_NONBLOCK) failed: ") + std::strerror(errno));
    }

    // wolfSSL server context
    wolfSSL_Init();
    WOLFSSL_CTX* ssl_ctx = wolfSSL_CTX_new(wolfTLS_server_method());
    if (!ssl_ctx) {
        ::close(listen_fd);
        throw std::runtime_error("quic_accept: wolfSSL_CTX_new() failed");
    }

    if (ngtcp2_crypto_wolfssl_configure_server_context(ssl_ctx) != 0) {
        wolfSSL_CTX_free(ssl_ctx);
        ::close(listen_fd);
        throw std::runtime_error(
            "quic_accept: ngtcp2_crypto_wolfssl_configure_server_context() failed");
    }

    if (wolfSSL_CTX_use_certificate_chain_file(ssl_ctx, cert_path.c_str()) != WOLFSSL_SUCCESS) {
        wolfSSL_CTX_free(ssl_ctx);
        ::close(listen_fd);
        throw std::runtime_error("quic_accept: failed to load cert: " + cert_path);
    }

    if (wolfSSL_CTX_use_PrivateKey_file(
        ssl_ctx, key_path.c_str(), WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
        wolfSSL_CTX_free(ssl_ctx);
        ::close(listen_fd);
        throw std::runtime_error("quic_accept: failed to load key: " + key_path);
    }

    WOLFSSL* ssl = wolfSSL_new(ssl_ctx);
    if (!ssl) {
        wolfSSL_CTX_free(ssl_ctx);
        ::close(listen_fd);
        throw std::runtime_error("quic_accept: wolfSSL_new() failed");
    }

    // must match the client's list exactly: bare name, wolfSSL length-prefixes
    if (wolfSSL_UseALPN(ssl, const_cast<char*>("embr"), 4,
                        WOLFSSL_ALPN_FAILED_ON_MISMATCH) != WOLFSSL_SUCCESS) {
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ssl_ctx);
        ::close(listen_fd);
        throw std::runtime_error("quic_accept: wolfSSL_UseALPN() failed");
    }

    wolfSSL_set_quic_transport_version(ssl, 0x39); // RFC 9001 codepoint
    wolfSSL_set_accept_state(ssl);

    // transport must exist before conn: ngtcp2 has no conn-level user_data setter
    QuicTransport* qt = nullptr;
    try {
        qt = new QuicTransport(listen_fd, ssl, ssl_ctx);
    } catch (...) {
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ssl_ctx);
        ::close(listen_fd);
        throw;
    }
    auto transport = std::unique_ptr<Transport>(qt);

    // dcid = the client's scid (where we send), scid = fresh, ours
    ngtcp2_cid dcid{};
    ngtcp2_cid scid{};
    ngtcp2_cid_init(&dcid, vc.scid, vc.scidlen);
    make_cid(&scid, NGTCP2_MAX_CIDLEN);

    // local half from transport's cached getsockname, not a fresh sockaddr
    ngtcp2_path path{};
    path.local.addr = reinterpret_cast<sockaddr*>(&qt->local_addr_);
    path.local.addrlen = qt->local_len_;
    path.remote.addr = reinterpret_cast<sockaddr*>(&peer_addr);
    path.remote.addrlen = peer_len;

    ngtcp2_settings settings{};
    ngtcp2_settings_default(&settings);
    settings.initial_ts = timestamp_ns();
    settings.log_printf = std::getenv("EMBR_QUIC_LOG") ? log_printf : nullptr;

    ngtcp2_transport_params params{};
    ngtcp2_transport_params_default(&params);
    params.max_idle_timeout = 30 * NGTCP2_SECONDS;
    params.initial_max_stream_data_bidi_local = 4 * 1024 * 1024;
    params.initial_max_stream_data_bidi_remote = 4 * 1024 * 1024;
    params.initial_max_data = 8 * 1024 * 1024;
    params.initial_max_streams_bidi = 1; // credit the client's open_bidi_stream needs

    // client checks this against the dcid it invented for us
    ngtcp2_cid_init(&params.original_dcid, vc.dcid, vc.dcidlen);
    params.original_dcid_present = 1;

    // recv_client_initial replaces client_initial; no recv_retry, servers send them
    ngtcp2_callbacks callbacks{};
    callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.update_key = ngtcp2_crypto_update_key_cb;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
    callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    callbacks.handshake_completed = QuicTransport::on_handshake_completed;
    callbacks.stream_open = QuicTransport::on_stream_open;
    callbacks.recv_stream_data = QuicTransport::on_recv_stream_data;
    callbacks.rand = QuicTransport::on_rand;
    callbacks.get_new_connection_id = QuicTransport::get_new_connection_id;
    callbacks.acked_stream_data_offset = QuicTransport::on_acked_stream_data_offset;

    ngtcp2_conn* conn = nullptr;
    rv = ngtcp2_conn_server_new(&conn, &dcid, &scid,
                                &path, vc.version,
                                &callbacks, &settings,
                                &params, nullptr, qt);
    if (rv != 0) {
        // transport owns ssl/ssl_ctx/listen_fd now, destructor frees them
        throw std::runtime_error(
            std::string("quic_accept: ngtcp2_conn_server_new() failed: ") + ngtcp2_strerror(rv));
    }
    qt->attach_conn(conn);

    // the Initial arrived before conn existed
    ngtcp2_pkt_info pi{};
    if (qt->feed_data(first_pkt, static_cast<size_t>(nread), &path, &pi) != 0) {
        throw std::runtime_error("quic_accept: first packet rejected");
    }

    if (qt->run_handshake() != 0) {
        throw std::runtime_error("quic_accept: handshake failed");
    }

    // stream_id stays -1: the client opens the stream after its handshake completes
    // recv() pump to discover it
    return transport;
}