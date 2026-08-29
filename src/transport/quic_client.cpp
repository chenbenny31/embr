//
// Created by benny on 6/20/26.
//

#include "quic_client.hpp"
#include "quic_transport.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <cstdarg>
#include <cstdio>
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

}

std::unique_ptr<Transport> quic_connect(const std::string& host, uint16_t port) {
    // UDP socket
    int udp_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_fd < 0) {
        throw std::runtime_error(
            std::string("quic_connect: socket() failed: ") + std::strerror(errno));
    }

    sockaddr_in peer_addr{};
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &peer_addr.sin_addr) != 1) {
        ::close(udp_fd);
        throw std::runtime_error("quic_connect: invalid address: " + host);
    }

    // connect on UDP: sets default peer, let recvmsg filter by source
    // also assigns the local addr the path depends on
    if (::connect(udp_fd,
                  reinterpret_cast<sockaddr*>(&peer_addr),
                  sizeof(peer_addr)) < 0) {
        ::close(udp_fd);
        throw std::runtime_error(
            std::string("quic_connect: connect() failed: ") + std::strerror(errno));
    }

    // non-blocking so pump_once() can select()
    if (::fcntl(udp_fd, F_SETFL, O_NONBLOCK) < 0) {
        ::close(udp_fd);
        throw std::runtime_error(
            std::string("quic_connect: fcntl(O_NONBLOCK) failed: ") + std::strerror(errno));
    }

    // wolfSSL client context
    wolfSSL_Init();
    WOLFSSL_CTX* ssl_ctx = wolfSSL_CTX_new(wolfTLS_client_method());
    if (!ssl_ctx) {
        ::close(udp_fd);
        throw std::runtime_error("quic_connect: wolfSSL_CTX_new() failed");
    }

    if (ngtcp2_crypto_wolfssl_configure_client_context(ssl_ctx) != 0) {
        wolfSSL_CTX_free(ssl_ctx);
        ::close(udp_fd);
        throw std::runtime_error(
            "quic_connect: ngtcp2_crypto_wolfssl_configure_client_context() failed");
    }

    // self-signed server cert, both endpoints ours
    wolfSSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, nullptr);

    WOLFSSL* ssl = wolfSSL_new(ssl_ctx);
    if (!ssl) {
        wolfSSL_CTX_free(ssl_ctx);
        ::close(udp_fd);
        throw std::runtime_error("quic_connect: wolfSSL_new() failed");
    }

    wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME,
                   host.c_str(), static_cast<uint16_t>(host.size()));

    // RFC 9001 8.1 requires ALPN; wolfSSL length-prefixes internally, wire from bare name
    if (wolfSSL_UseALPN(ssl, const_cast<char*>("embr"), 4,
                        WOLFSSL_ALPN_FAILED_ON_MISMATCH) != WOLFSSL_SUCCESS) {
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ssl_ctx);
        ::close(udp_fd);
        throw std::runtime_error("quic_connect: wolfSSL_UseALPN() failed");
    }

    wolfSSL_set_quic_transport_version(ssl, 0x39); // RFC 9001 codepoint

    // transport must exist before conn: ngtcp2 has no conn-level user_data setter
    QuicTransport* qt = nullptr;
    try {
        qt = new QuicTransport(udp_fd, ssl, ssl_ctx);
    } catch (...) {
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ssl_ctx);
        ::close(udp_fd);
        throw;
    }
    auto transport = std::unique_ptr<Transport>(qt);

    ngtcp2_cid dcid{}; // dst conn id (server)
    ngtcp2_cid scid{}; // src conn id (client)
    make_cid(&dcid, NGTCP2_MAX_CIDLEN);
    make_cid(&scid, NGTCP2_MAX_CIDLEN);

    // local half from transport's cached getsockname, not a fresh sockaddr
    ngtcp2_path path{};
    path.local.addr = reinterpret_cast<sockaddr*>(&qt->local_addr_);
    path.local.addrlen = qt->local_len_;
    path.remote.addr = reinterpret_cast<sockaddr*>(&peer_addr);
    path.remote.addrlen = sizeof(peer_addr);

    ngtcp2_settings settings{};
    ngtcp2_settings_default(&settings);
    settings.initial_ts = timestamp_ns();
    settings.log_printf = log_printf;

    ngtcp2_transport_params params{};
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_data = 1 * 1024 * 1024;
    params.initial_max_streams_bidi = 1;

    // every ngtcp2_crypto_* entry is mandatory; omitting one fails the handshake silently
    // client_initial makes the first drain_packets() emit the ClientHello
    ngtcp2_callbacks callbacks{};
    callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
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

    ngtcp2_conn* conn = nullptr;
    int rv = ngtcp2_conn_client_new(&conn, &dcid, &scid,
                                    &path, NGTCP2_PROTO_VER_V1,
                                    &callbacks, &settings,
                                    &params, nullptr, qt);
    if (rv != 0) {
        // transport owns ssl/ssl_ctx/udp_fd now, destructor frees them
        throw std::runtime_error(
            std::string("quic_connect: ngtcp2_conn_client_new() failed: ") + ngtcp2_strerror(rv));
    }
    qt->attach_conn(conn);

    if (qt->run_handshake() != 0) {
        throw std::runtime_error("quic_connect: handshake failed");
    }

    // client opens the stream; stream_open never fires for the local side
    // STREAM_ID_BLOCKED if the server didn't grant bidi credit
    int64_t stream_id = -1;
    rv = ngtcp2_conn_open_bidi_stream(conn, &stream_id, nullptr);
    if (rv != 0) {
        throw std::runtime_error(
            std::string("quic_connect: open_bidi_stream failed: ") + ngtcp2_strerror(rv));
    }
    qt->stream_id_ = stream_id;

    return transport;
}
