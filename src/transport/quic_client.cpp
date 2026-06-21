//
// Created by benny on 6/20/26.
//

#include "quic_client.hpp"
#include "quic_transport.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>
#include <wolfssl/ssl.h>
#include <wolfssl/options.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

static uint64_t timestamp_ns() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

static void make_cid(ngtcp2_cid* cid, size_t len) {
    cid->datalen = len;
    for (size_t i = 0; i < len; i++) {
        cid->data[i] = static_cast<uint8_t>(rand() % 256);
    }
}

}

std::unique_ptr<Transport> quic_connect(const std::string& host, uint16_t port) {
    // UDP socket
    int udp_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_fd < 0) {
        throw std::runtime_error(
            std::string("quic_connect: scoket() failed: ") + std::strerror(errno));
    }

    sockaddr_in peer_addr{};
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &peer_addr.sin_addr) != 1) {
        ::close(udp_fd);
        throw std::runtime_error("quic_connect: invalid address: " + host);
    }

    // connect on UDP: sets default peer, let recvfrom filter by source
    if (::connect(udp_fd,
                  reinterpret_cast<struct sockaddr*>(&peer_addr),
                  sizeof(peer_addr)) < 0) {
        ::close(udp_fd);
        throw std::runtime_error(
            std::string("quic_connect: connect() failed: ") + std::strerror(errno));
    }

    // non-blocking, QuicTransport::run_handshake() can poll
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

    // configure context for ngtcp2 QUIC crypto, set QUIC callbacks on ssl_ctx
    if (ngtcp2_crypto_wolfssl_configure_client_context(ssl_ctx) != 0) {
        wolfSSL_CTX_free(ssl_ctx);
        ::close(udp_fd);
        throw std::runtime_error("quic_connect: ngtcp2_crypto_wolfssl_configure_client_context() failed");
    }

    wolfSSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, nullptr);

    WOLFSSL* ssl = wolfSSL_new(ssl_ctx);
    if (!ssl) {
        wolfSSL_CTX_free(ssl_ctx);
        ::close(udp_fd);
        throw std::runtime_error("quic_connect: wolfSSL_new() failed");
    }

    // SNI optionally required, use host as server name
    wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME,
                   host.c_str(), static_cast<uint16_t>(host.size()));

    // ngtcp2 client connection
    ngtcp2_cid dcid{}; // dst conn id (server)
    ngtcp2_cid scid{}; // src conn id (client)
    make_cid(&dcid, NGTCP2_MAX_CIDLEN);
    make_cid(&scid, NGTCP2_MAX_CIDLEN);

    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = 0;

    ngtcp2_path path{};
    path.local.addrlen = sizeof(local_addr);
    path.local.addr = reinterpret_cast<sockaddr*>(&local_addr);
    path.remote.addrlen = sizeof(peer_addr);
    path.remote.addr = reinterpret_cast<sockaddr*>(&peer_addr);

    ngtcp2_settings settings{};
    ngtcp2_settings_default(&settings);
    settings.initial_ts = timestamp_ns();

    ngtcp2_transport_params params{};
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_data = 1 * 1024 * 1024;
    params.initial_max_streams_bidi = 1;

    ngtcp2_callbacks callbacks{};
    callbacks.recv_crypto_data = QuicTransport::on_recv_crypto_data;
    callbacks.handshake_completed = QuicTransport::on_handshake_completed;
    callbacks.stream_open = QuicTransport::on_stream_open;
    callbacks.recv_stream_data = QuicTransport::on_recv_stream_data;
    callbacks.rand = QuicTransport::on_rand;
    callbacks.get_new_connection_id = QuicTransport::get_new_connection_id;

    ngtcp2_conn* conn = nullptr;
    int rv = ngtcp2_conn_client_new(&conn, &dcid, &scid,
                                    &path, NGTCP2_PROTO_VER_V1,
                                    &callbacks, &settings,
                                    &params, nullptr, ssl);
    if (rv != 0) {
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ssl_ctx);
        ::close(udp_fd);
        throw std::runtime_error(
            std::string("quic_connect: ngtcp2_conn_client_new() failed: ") + ngtcp2_strerror(rv));
    }

    // wrap into QuicTransport + run handshake
    auto transport = std::unique_ptr<Transport>(
        new QuicTransport(udp_fd, conn, ssl, ssl_ctx));

    auto* qt = static_cast<QuicTransport*>(transport.get());
    if (qt->run_handshake() != 0) {
        throw std::runtime_error("quic_connect: handshake failed");
    }

    return transport;
}
