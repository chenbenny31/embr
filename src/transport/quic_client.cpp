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
#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <cstdarg>
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

}

std::unique_ptr<Transport> quic_connect(const std::string& host, uint16_t port,
                                        const std::string& cert_hash) {
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

    // rmem_default is (~157 datagrams at 1350 B) against 10-packet bursts and an AEAD-bound recv
    const int sock_buf = 4 * 1024 * 1024; // rmem_max/wmem_max, no sysctl
    ::setsockopt(udp_fd, SOL_SOCKET, SO_RCVBUF, &sock_buf, sizeof(sock_buf));
    ::setsockopt(udp_fd, SOL_SOCKET, SO_SNDBUF, &sock_buf, sizeof(sock_buf));

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
    settings.handshake_timeout = QUIC_HANDSHAKE_TIMEOUT_NS;
    settings.log_printf = std::getenv("EMBR_QUIC_LOG") ? log_printf : nullptr;
    // ngtcp2 starts at 1200 bytes and grows only by a probe that fits the packet buffer; its own
    // first probe (1406) does not, so name the size: one 1350-byte probe, 1200 stays if it is lost
    settings.max_tx_udp_payload_size = QUIC_MAX_PKTLEN;
    settings.pmtud_probes = &QUIC_PMTUD_PROBE;
    settings.pmtud_probeslen = 1;
    // EMBR_QUIC_ACK_THRESH=N: acknowledge every N packets, not every 2; the peer's GSO batches follow it
    if (const char* thresh = std::getenv("EMBR_QUIC_ACK_THRESH")) {
        settings.ack_thresh = static_cast<size_t>(std::clamp(std::atoi(thresh), 1, 100));
    }

    ngtcp2_transport_params params{};
    ngtcp2_transport_params_default(&params);
    params.max_idle_timeout = 30 * NGTCP2_SECONDS;
    params.initial_max_stream_data_bidi_local = 4 * 1024 * 1024;
    params.initial_max_stream_data_bidi_remote = 4 * 1024 * 1024;
    params.initial_max_data = 8 * 1024 * 1024;
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
    callbacks.acked_stream_data_offset = QuicTransport::on_acked_stream_data_offset;

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
        if (qt->handshake_timed_out_) {
            const std::string where = host + ":" + std::to_string(port);
            const std::string secs = std::to_string(QUIC_HANDSHAKE_TIMEOUT_NS / 1'000'000'000ULL) + " s";
            throw std::runtime_error(qt->refused_seen_
                ? "quic_connect: nothing is listening at " + where +
                  " (connection refused) - gave up after " + secs
                : "quic_connect: no answer from " + where + " within " + secs);
        }
        throw std::runtime_error("quic_connect: handshake failed");
    }

    // nothing has been sent on a stream yet: a sender with the wrong certificate learns nothing
    if (!cert_hash.empty()) {
        WOLFSSL_X509* peer = wolfSSL_get_peer_certificate(ssl);
        const std::string actual = cert_sha256_hex(peer);
        if (peer) { wolfSSL_X509_free(peer); }
        if (actual.compare(0, cert_hash.size(), cert_hash) != 0) {
            throw std::runtime_error("quic_connect: the sender's certificate does not match "
                                     "the token's cert hash - not the sender this token names");
        }
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
