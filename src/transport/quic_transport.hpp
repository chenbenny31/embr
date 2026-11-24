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
#include "quic_egress.hpp"
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
inline constexpr uint16_t QUIC_PMTUD_PROBE = QUIC_MAX_PKTLEN; // the one size path MTU discovery tries
inline constexpr size_t QUIC_MAX_RECV_PKTLEN = 1500; // independent of send cap
// UDP GRO: the kernel hands a run of same-size datagrams over as one read of at most 64 KiB
inline constexpr size_t QUIC_GRO_BUFLEN = 65536;
inline constexpr unsigned QUIC_RECV_BURST = 64;      // packets read before the ACKs for them are written
inline constexpr size_t QUIC_MAX_BURST = 10;         // datagrams per write cycle
inline constexpr uint64_t QUIC_CLOSE_LINER_NS = 1'000'000'000ULL; // graceful close bound: 100ms RTT
inline constexpr uint64_t QUIC_HANDSHAKE_TIMEOUT_NS = 10'000'000'000ULL; // give up connecting after this
// SEND_ZC slots per conn: ~ 105 KiB of memlock; slots recycle on NOTIF (NIC TX done) not on ACK
inline constexpr unsigned QUIC_ZC_SLOTS = 64;
// UDP GSO: up to this many same-size packets leave in one send; a SEND_ZC slot then holds one batch
// 48 x 1350 B is the most one UDP send carries (65,507 B)
inline constexpr unsigned QUIC_GSO_SEGS = 48;
// packets the SEND_ZC slab holds with GSO, in whole slots: 3 x 48 x 1350 B = 190 KiB per conn
inline constexpr unsigned QUIC_ZC_GSO_PACKETS = 128;
// a send shorter than a full packet is copied by sendmsg: its SEND_ZC slot would stay busy until the
// kernel lets go of the packet, and a receiver that only writes ACKs then waits for slots
inline constexpr size_t QUIC_ZC_MIN_BYTES = 1200;
// the kernel reports whether it copied a SEND_ZC send after all (loopback, veth, a packet capture):
// when it copied every one of the first this many, the connection goes over to sendmsg
// no more than the slots a connection has: on a path that holds packets a longer probe waits for them
inline constexpr uint64_t QUIC_ZC_PROBE_SENDS = 3;
// flow-control credit: at 4 MiB the sender ran out of it once per round trip, about 3 MiB each
inline constexpr uint64_t QUIC_STREAM_WINDOW = 16 * 1024 * 1024;
inline constexpr uint64_t QUIC_CONN_WINDOW = 2 * QUIC_STREAM_WINDOW;
// asked for each direction: rmem_default holds ~157 packets, against bursts and an AEAD-bound receiver
inline constexpr int QUIC_SOCKET_BUFFER = 4 * 1024 * 1024;

// lowercase hex SHA-256 of a certificate's DER encoding; empty if cert is null
std::string cert_sha256_hex(WOLFSSL_X509* cert);

// asks for bytes of send and of receive buffer; returns what the kernel granted, the smaller of the two
// (it caps a request at net.core.rmem_max / wmem_max without an error)
int set_socket_buffers(int fd, int bytes);
// QUIC_SOCKET_BUFFER for a QUIC socket; when the kernel grants less, says so on stderr with the
// command that lifts the limit, once per process
void set_quic_socket_buffers(int fd);

// QUIC implementation of Transport
// construct only via factories: quic_connect, quic_accept
//
// send_file: mmap datav + ngtcp2 datav->dest assembly (1 copy) + in-place AEAD on dest
// recv_file: ngtcp2 stream re-assembly -> mmap dest (1 copy)
class QuicTransport final : public Transport {
public:
    // --- control plane ---
    ssize_t send(const uint8_t* buf, size_t len) override;
    ssize_t recv(uint8_t* buf, size_t len) override;

    size_t unacked_bytes() const;

    // null when EMBR_QUIC_EGRESS=sendmsg or io_uring in unavailable
    const ZcEgress* zc_egress() const { return zc_.get(); }

    // packets written and send calls made; datagrams > sends means GSO batched them
    // bytes / datagrams is the packet size the path settled on
    struct SendStats {
        uint64_t datagrams{0};
        uint64_t sends{0};
        uint64_t bytes{0};
        uint64_t by_sendmsg{0}; // sends that left through sendmsg; the others went through SEND_ZC
        unsigned largest{0};    // packets in the largest send
    };
    const SendStats& send_stats() const { return send_stats_; }
    bool gso() const { return gso_; } // false: EMBR_QUIC_GSO=0 or the kernel has no UDP_SEGMENT

    // packets received and recvmsg calls that returned them; datagrams > reads means reads were batched
    struct RecvStats {
        uint64_t datagrams{0};
        uint64_t reads{0};
        uint64_t bytes{0};
    };
    const RecvStats& recv_stats() const { return recv_stats_; }
    bool gro() const { return gro_; } // false: EMBR_QUIC_GRO=0, the kernel has no UDP_GRO, or nothing read yet

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
    int abandon(); // hand the socket back and go quiet: this connection attempt is discarded

    int udp_fd_;
    ngtcp2_conn* conn_{nullptr};
    WOLFSSL* ssl_;
    WOLFSSL_CTX* ssl_ctx_;
    ngtcp2_crypto_conn_ref crypto_conn_ref_;

    int64_t stream_id_{-1}; // single bidi stream, shared by control + data plane
    bool peer_closed_{false}; // peer's NO_ERROR close: recv() returns 0 once drained, like TCP's FIN
    bool closed_{false}; // close sent or silent teardown, set for every transition
    bool handshake_timed_out_{false}; // why closed_: the factories turn it into the error text
    bool refused_seen_{false}; // an ICMP port-unreachable came back at some point
    std::vector<uint8_t> recv_buf_;

    // recv_file dest: on_recv_stream_data lands bytes here
    uint8_t* recv_dest_{nullptr};
    size_t recv_dest_len_{0};
    size_t recv_dest_got_{0};

    // egress seam: one assembly dest + one commit per datagram
    enum class SendResult { ok, blocked, failed };

    uint8_t packet_buf_[QUIC_MAX_PKTLEN]; // sendmsg mode, sync CLOSE in all modes
    std::unique_ptr<ZcEgress> zc_; // created at first begin_packet()
    bool egress_init_{false};
    void init_egress();
    uint8_t* begin_packet();
    SendResult send_packet(size_t n);
    bool flush_packets(); // false: the open batch failed to send, errno says why

    // GSO batch: same-size packets laid end to end, handed to the kernel as one send
    // without GSO a batch is always one packet and every path below is today's
    bool gso_{false};
    unsigned gso_segs_{QUIC_GSO_SEGS}; // EMBR_QUIC_GSO_SEGS
    bool zc_always_{false}; // EMBR_QUIC_EGRESS=zc: every send through SEND_ZC
    bool zc_copied_{false}; // the kernel copied the first SEND_ZC sends: sendmsg from here on
    std::vector<uint8_t> batch_buf_; // sendmsg mode with GSO
    uint8_t* batch_base_{nullptr};   // a SEND_ZC slot, batch_buf_ or packet_buf_
    size_t batch_len_{0};
    size_t batch_seg_{0};            // size of the batch's first packet = its segment size
    unsigned batch_pkts_{0};
    size_t sock_seg_{0};             // UDP_SEGMENT the socket holds now, 0 = unset
    SendResult send_batch();
    SendStats send_stats_;

    // ingress: read what the socket holds, feed every packet, then write the ACKs once
    bool ingress_init_{false};
    bool gro_{false};
    std::vector<uint8_t> read_buf_;
    RecvStats recv_stats_;
    void init_ingress();
    int read_packets(); // packets read and handed to ngtcp2; 0: nothing was waiting; -1: the connection is over

    // drive ones retained block until ngtcp2 accepts every byte
    ssize_t write_stream(const uint8_t* base, size_t len);

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

    void close_connection(int liberr); // fire-once, liberr==0 (app NO_ERROR), until unacked_ drains

    // one read_packets -> drain_packets cycle, waiting for the socket only when it is empty; until caps the wait
    int pump_once(ngtcp2_tstamp until = UINT64_MAX);

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
                                                  uint16_t port,
                                                  const std::string& cert_hash);
    friend std::unique_ptr<Transport> quic_accept(int listen_fd,
                                                  const std::string& cert_path,
                                                  const std::string& key_path);
    friend std::unique_ptr<Transport> quic_accept_one(int listen_fd,
                                                      const std::string& cert_path,
                                                      const std::string& key_path);
};