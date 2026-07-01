//
// Created by benny on 6/20/26.
//

// Dependencies:
//   ngtcp2 >= 1.22 --with-wolfssl (libngtcp2, libngtcp2_crypto_wolfssl)
//   woflSSL native API --enable-quic, NO --enable-opensslextra

#pragma once

#include "transport.hpp"
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>
#include <wolfssl/ssl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

inline constexpr size_t QUIC_MAX_PKTLEN = 1350; // whole datagram including header
inline constexpr size_t QUIC_MAX_BURST = 10; // max num of datagrams drained per write cycle

// QUIC implementation of Transport
// construct only via factories: quic_connect, quic_accept
//
// send_file: mmap datav + ngtcp2 datav->dest assembly (1 copy) + in-place AEAD on dest
// recv_file: ngtcp2 stream re-assembly + SHA-256 verify
class QuicTransport final : public Transport {
public:
    // --- control plane ---
    ssize_t send(const uint8_t* buf, size_t len) override;
    ssize_t recv(uint8_t* buf, size_t len) override;

    // --- data plane ---
    void send_file(int file_fd, uint64_t offset, size_t len) override;
    void recv_file(int file_fd, uint64_t offset, size_t len) override;

    ~QuicTransport();

    // Non-copyable, non-movable, owns ngtcp2_conn* and WOLFSLL*
    QuicTransport(const QuicTransport&) = delete;
    QuicTransport& operator=(const QuicTransport&) = delete;

private:
    QuicTransport(int udp_fd, ngtcp2_conn* conn, WOLFSSL* ssl, WOLFSSL_CTX* ssl_ctx);

    int udp_fd_;
    ngtcp2_conn* conn_;
    WOLFSSL* ssl_;
    WOLFSSL_CTX* ssl_ctx_;
    ngtcp2_crypto_conn_ref crypto_conn_ref_;

    int64_t stream_id_{-1}; // single bidi stream, shared by control + data plane
    std::vector<uint8_t> recv_buf_;

    sockaddr_storage peer_addr_{};
    socklen_t peer_addrlen_{0};

    // --- I/O helpers ---
    int feed_data(const uint8_t* data,
                  size_t datalen,
                  const ngtcp2_path* path,
                  const ngtcp2_pkt_info* pi);

    int drain_packets();

    int pump_once(const sockaddr_storage& local_addr, socklen_t local_len);

    int run_handshake();

    // --- ngtcp2 callbacks ---
    static int on_recv_crypto_data(ngtcp2_conn* conn,
                                   ngtcp2_encryption_level level,
                                   uint64_t offset,
                                   const uint8_t* data,
                                   size_t datalen,
                                   void* user_data);
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
    friend std::unique_ptr<Transport> quic_accept(int listen_fd);
};