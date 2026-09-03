//
// Created by benny on 6/20/26.
//

// Dependencies:
//   ngtcp2 >= 1.22 --with-wolfssl (libngtcp2, libngtcp2_crypto_wolfssl)
//   wolfSSL --enable-quic --enable-opensslextra --enable-aesecb
//   (crypto_wolfssl needs wolfSSL_EVP_aes_{128,256}_ecb from the last two)

#pragma once

#include "transport.hpp"
#include "core/protocol.hpp"
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <deque>

inline constexpr size_t QUIC_MAX_PKTLEN = 1350;      // datagram local sends include header
inline constexpr size_t QUIC_MAX_RECV_PKTLEN = 1500; // independent of send cap
inline constexpr size_t QUIC_MAX_BURST = 10;         // datagrams per write cycle

// QUIC implementation of Transport
// construct only via factories: quic_connect, quic_accept
//
// send_file: mmap datav + ngtcp2 datav->dest assembly (1 copy) + in-place AEAD on dest
// recv_file: ngtcp2 stream re-assembly
class QuicTransport final : public Transport {
public:
    // --- control plane ---
    ssize_t send(const uint8_t* buf, size_t len) override;
    ssize_t recv(uint8_t* buf, size_t len) override;

    // QUIC-specific half-close: empty STREAM+FIN from the local endpoint
    int send_fin();

    size_t unacked_bytes() const;

    // --- data plane ---
    void send_file(int file_fd, uint64_t offset, size_t len) override;
    void recv_file(int file_fd, uint64_t offset, size_t len) override;

    ~QuicTransport();

    // Non-copyable, non-movable, owns ngtcp2_conn* and WOLFSSL*
    QuicTransport(const QuicTransport&) = delete;
    QuicTransport& operator=(const QuicTransport&) = delete;

private:
    QuicTransport(int udp_fd, WOLFSSL* ssl, WOLFSSL_CTX* ssl_ctx);
    void attach_conn(ngtcp2_conn* conn); // factory calls after conn_*_new

    int udp_fd_;
    ngtcp2_conn* conn_{nullptr};
    WOLFSSL* ssl_;
    WOLFSSL_CTX* ssl_ctx_;
    ngtcp2_crypto_conn_ref crypto_conn_ref_;

    int64_t stream_id_{-1}; // single bidi stream, shared by control + data plane
    bool stream_fin_received_{false}; // peer FIN, recv() returns 0 once drained
    bool fin_sent_{false}; // local FIN, send_file() is idempotent
    bool closed_{false}; // close sent or silent teardown, set for every transition
    std::vector<uint8_t> recv_buf_;

    // egress seam: one assembly dest + one commit per datagram
    enum class SendResult { ok, blocked, failed };

    uint8_t packet_buf_[QUIC_MAX_PKTLEN];
    uint8_t* begin_packet();
    SendResult send_packet(size_t n);
    void flush_packets();

    // ngtcp2 re-encodes lost STREAM frames from the caller's ptr
    // block must outlive send() until acked
    struct Unacked {
        uint64_t end_offset;
        Buffer buf;
    };
    std::deque<Unacked> unacked_;
    uint64_t stream_offset_{0};

    static int on_acked_stream_data_offset(ngtcp2_conn* conn,
                                           int64_t stream_id,
                                           uint64_t offset,
                                           uint64_t datalen,
                                           void* user_data,
                                           void* stream_user_data);

    sockaddr_storage local_addr_{};
    socklen_t local_len_{0};

    // --- I/O helpers ---
    int feed_data(const uint8_t* data,
                  size_t datalen,
                  const ngtcp2_path* path,
                  const ngtcp2_pkt_info* pi);

    int drain_packets();

    void close_connection(int liberr); // fire-once, liberr==0 means app NO_ERROR

    int pump_once(); // one recvmsg -> feed_data -> drain_packets cycle

    int run_handshake();

    // --- ngtcp2 callbacks ---
    static int on_handshake_completed(ngtcp2_conn* conn,
                                       void* user_data);
    static int on_stream_open(ngtcp2_conn* conn,
                              int64_t stream_id,
                              void* user_data);
    static int on_recv_stream_data(ngtcp2_conn* conn,
                                   uint32_t flags,
                                   int64_t stream_id,
                                   uint64_t offset,
                                   const uint8_t* data,
                                   size_t datalen,
                                   void* user_data,
                                   void* stream_user_data);
    static void on_rand(uint8_t* dest,
                       size_t destlen,
                       const ngtcp2_rand_ctx* rand_ctx);
    static int get_new_connection_id(ngtcp2_conn* conn,
                                     ngtcp2_cid* cid,
                                     uint8_t* token,
                                     size_t cidlen,
                                     void* user_data);

    friend std::unique_ptr<Transport> quic_connect(const std::string& host,
                                                  uint16_t port);
    friend std::unique_ptr<Transport> quic_accept(int listen_fd,
                                                  const std::string& cert_path,
                                                  const std::string& key_path);
};