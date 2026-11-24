//
// Created by benny on 6/22/26.
//

#include "quic_transport.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/udp.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <ctime>
#include <unistd.h>
#include <utility>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103 // libc headers older than the kernel feature
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif

namespace {

static ngtcp2_tstamp now_ns() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<ngtcp2_tstamp>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<ngtcp2_tstamp>(ts.tv_nsec);
}

}

QuicTransport::QuicTransport(int udp_fd, WOLFSSL* ssl, WOLFSSL_CTX* ssl_ctx)
        : udp_fd_(udp_fd), ssl_(ssl), ssl_ctx_(ssl_ctx) {

    // ngtcp2_crypto finds conn via wolfSSL_get_app_data(ssl)
    crypto_conn_ref_.get_conn = [](ngtcp2_crypto_conn_ref* ref) -> ngtcp2_conn* {
       return static_cast<QuicTransport*>(ref->user_data)->conn_;
    };
    crypto_conn_ref_.user_data = this;
    wolfSSL_set_app_data(ssl_, &crypto_conn_ref_);

    // factory must bind/connect udp_fd_ before construction
    // this addr is also the path factory hands conn_*_new, must match
    local_len_ = sizeof(local_addr_);
    if (::getsockname(udp_fd_, reinterpret_cast<sockaddr*>(&local_addr_), &local_len_) < 0) {
        throw std::runtime_error(
            std::string("QuicTransport: getsockname failed: ") + std::strerror(errno));
    }
}

void QuicTransport::attach_conn(ngtcp2_conn* conn) {
    conn_ = conn;
    ngtcp2_conn_set_tls_native_handle(conn_, ssl_);
}

std::string cert_sha256_hex(WOLFSSL_X509* cert) {
    int der_len = 0;
    const unsigned char* der = cert ? wolfSSL_X509_get_der(cert, &der_len) : nullptr;
    unsigned char digest[WC_SHA256_DIGEST_SIZE];
    if (!der || der_len <= 0 ||
        wc_Sha256Hash(der, static_cast<word32>(der_len), digest) != 0) { return {}; }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(2 * sizeof(digest));
    for (unsigned char byte : digest) { out += hex[byte >> 4]; out += hex[byte & 0x0f]; }
    return out;
}

int set_socket_buffers(int fd, int bytes) {
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
    // the kernel reports twice what it reserved for the caller
    int recv_buf = 0;
    int send_buf = 0;
    socklen_t len = sizeof(recv_buf);
    ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &recv_buf, &len);
    len = sizeof(send_buf);
    ::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buf, &len);
    return std::min(recv_buf, send_buf) / 2;
}

void set_quic_socket_buffers(int fd) {
    static std::atomic_flag told;
    const int granted = set_socket_buffers(fd, QUIC_SOCKET_BUFFER);
    if (granted < QUIC_SOCKET_BUFFER && !told.test_and_set()) {
        std::fprintf(stderr, "[warn] UDP socket buffers: the kernel grants %d KiB of the %d KiB embr asks for; "
                             "QUIC works, and loses packets when the receiver falls behind\n"
                             "[warn] to lift the limit, with root: sudo sysctl -w net.core.rmem_max=%d "
                             "net.core.wmem_max=%d (install.sh sysctl also keeps it after a reboot)\n",
                     granted / 1024, QUIC_SOCKET_BUFFER / 1024, QUIC_SOCKET_BUFFER, QUIC_SOCKET_BUFFER);
    }
}

int QuicTransport::abandon() {
    closed_ = true; // the destructor then sends nothing
    return std::exchange(udp_fd_, -1);
}

QuicTransport::~QuicTransport() {
    if (conn_ && !closed_) { close_connection(0); } // graceful NO_ERROR
    if (conn_) { ngtcp2_conn_del(conn_); }
    if (ssl_) { wolfSSL_free(ssl_); }
    if (ssl_ctx_) { wolfSSL_CTX_free(ssl_ctx_); }
    // EMBR_QUIC_STATS=1: one line per conn; datagrams/sends is the GSO batch size,
    // copied==0 is the proof that NIC read pages
    if (send_stats_.sends > 0 && std::getenv("EMBR_QUIC_STATS")) {
        std::fprintf(stderr, "[quic-egress] mode=%s gso=%d datagrams=%llu sends=%llu bytes=%llu",
            zc_ ? "zc" : "sendmsg", gso_ ? 1 : 0, (unsigned long long)send_stats_.datagrams,
            (unsigned long long)send_stats_.sends, (unsigned long long)send_stats_.bytes);
        if (zc_) {
            const auto& s = zc_->stats();
            std::fprintf(stderr, " notifs=%llu copied=%llu fallback=%llu errors=%llu waits=%llu by_sendmsg=%llu registered=%d",
                (unsigned long long)s.notifs, (unsigned long long)s.copied,
                (unsigned long long)s.fallback, (unsigned long long)s.errors,
                (unsigned long long)s.waits, (unsigned long long)send_stats_.by_sendmsg,
                zc_->registered() ? 1 : 0);
        }
        std::fputc('\n', stderr);
    }
    // datagrams/reads is how many packets one recvmsg returned
    if (recv_stats_.reads > 0 && std::getenv("EMBR_QUIC_STATS")) {
        std::fprintf(stderr, "[quic-ingress] gro=%d datagrams=%llu reads=%llu bytes=%llu\n",
            gro_ ? 1 : 0, (unsigned long long)recv_stats_.datagrams,
            (unsigned long long)recv_stats_.reads, (unsigned long long)recv_stats_.bytes);
    }
    zc_.reset(); // drains in-flight SEND_ZC slots before the socket goes
    if (udp_fd_ >= 0) { ::close(udp_fd_); }
}

// --- control plane ---
ssize_t QuicTransport::send(const uint8_t* buf, size_t len) {
    if (closed_) {
        errno = ENOTCONN;
        return -1;
    }
    if (stream_id_ < 0) {
        throw std::runtime_error("QuicTransport::send: no open stream");
    }
    if (len == 0) { return 0; }

    // send_msg hands a stack header and a payload dies at return
    Buffer owned(len);
    std::memcpy(owned.get(), buf, len);
    unacked_.push_back(Unacked{stream_offset_ + len, std::move(owned)});

    return write_stream(unacked_.back().buf.get(), len);
}

ssize_t QuicTransport::recv(uint8_t* buf, size_t len) {
    // no stream_id_ guard: server is still -1 here, must pump to fire on_stream_open
    while (recv_buf_.empty() && !closed_) {
        if (pump_once() != 0) { break; } // teardown paths set closed_
    }

    // drain before classifying: buffered data outlives the conn
    if (!recv_buf_.empty()) {
        const size_t n = std::min(len, recv_buf_.size());
        std::memcpy(buf, recv_buf_.data(), n);
        recv_buf_.erase(recv_buf_.begin(),
                        recv_buf_.begin() + static_cast<ptrdiff_t>(n));
        return static_cast<ssize_t>(n);
    }

    if (peer_closed_) { return 0; } // orderly close, matches TCP recv()==0
    return -1;
}

// --- data plane ---
void QuicTransport::send_file(int file_fd, uint64_t offset, size_t len) {
    if (len == 0) { return; }
    if (closed_) { throw std::runtime_error("QuicTransport::send_file: connection closed"); }
    if (stream_id_ < 0) { throw std::runtime_error("QuicTransport::send_file: no open stream"); }

    // mmap offset must be page-aligned, delta locates the payload in mapping
    const uint64_t page = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
    const uint64_t base_off = offset & ~(page - 1);
    const size_t delta = static_cast<size_t>(offset - base_off);
    const size_t map_len = len + delta;

    void* map = ::mmap(nullptr, map_len, PROT_READ, MAP_PRIVATE,
                       file_fd, static_cast<off_t>(base_off));
    if (map == MAP_FAILED) {
        throw std::runtime_error(
            std::string("QuicTransport::send_file: mmap failed: ") + std::strerror(errno));
    }
    auto* map_base = static_cast<uint8_t*>(map);

    // same deque, same erase release: heap in send(), munmap here
    Buffer chunk(map_base + delta, len,
        [map_base, map_len](uint8_t*) { ::munmap(map_base, map_len); });
    unacked_.push_back(Unacked{stream_offset_ + len, std::move(chunk)});

    if (write_stream(unacked_.back().buf.get(), len) < 0) {
        throw std::runtime_error(
            std::string("QuicTransport::send_file: send failed: ") + std::strerror(errno));
    }
}

void QuicTransport::recv_file(int file_fd, uint64_t offset, size_t len) {
    if (len == 0) { return; }

    const uint64_t page = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
    const uint64_t base_off = offset & ~(page - 1);
    const size_t delta = static_cast<size_t>(offset - base_off);
    const size_t map_len = len + delta;

    // bytes land through a shared mapping: reserve the blocks first, a full disk is an error not SIGBUS
    const int reserve_rc = ::posix_fallocate(file_fd, static_cast<off_t>(offset),
                                             static_cast<off_t>(len));
    if (reserve_rc != 0) {
        throw std::runtime_error(
            std::string("QuicTransport::recv_file: cannot reserve disk space: ") +
            std::strerror(reserve_rc));
    }

    void* map = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                       file_fd, static_cast<off_t>(base_off));
    if (map == MAP_FAILED) {
        throw std::runtime_error(
            std::string("QuicTransport::recv_file: mmap failed: ") + std::strerror(errno));
    }
    auto* map_base = static_cast<uint8_t*>(map);

    // erased release also clears the callback ptr: it must not outlive the mapping
    Buffer dest(map_base, map_len,
        [this, map_len](uint8_t* p) { recv_dest_ = nullptr; ::munmap(p, map_len); });

    // bytes buffered before this call precede the file bytes in stream order
    size_t got = 0;
    if (!recv_buf_.empty()) {
        got = std::min(len, recv_buf_.size());
        std::memcpy(map_base + delta, recv_buf_.data(), got);
        recv_buf_.erase(recv_buf_.begin(), recv_buf_.begin() + static_cast<ptrdiff_t>(got));
    }

    recv_dest_ = map_base + delta;
    recv_dest_len_ = len;
    recv_dest_got_ = got;

    while (recv_dest_got_ < len) {
        if (closed_) {
            throw std::runtime_error("QuicTransport::recv_file: connection closed");
        }
        // one read can hold the last bytes and the peer's close after them
        if (pump_once() != 0 && recv_dest_got_ < len) {
            throw std::runtime_error("QuicTransport::recv_file: pump failed");
        }
    }
}

// --- I/O helpers ---
int QuicTransport::feed_data(const uint8_t* data,
                             size_t datalen,
                             const ngtcp2_path* path,
                             const ngtcp2_pkt_info* pi) {
    return ngtcp2_conn_read_pkt(conn_, path, pi, data, datalen, now_ns());
}

int QuicTransport::drain_packets() {
    if (closed_) { return 0; } // pump's own return carries the signal

    for (size_t i = 0; i < QUIC_MAX_BURST; i++) {
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi{};

        // stream_id -1 + null datav: ACK/CRYPTO/PING only
        const ngtcp2_ssize nwrite =
            ngtcp2_conn_writev_stream(conn_, &ps.path, &pi,
                                      begin_packet(), QUIC_MAX_PKTLEN,
                                      nullptr,
                                      NGTCP2_WRITE_STREAM_FLAG_NONE,
                                      -1,
                                      nullptr, 0,
                                      now_ns());
        if (nwrite == 0) { break; }
        if (nwrite < 0) { return static_cast<int>(nwrite); }

        const SendResult sr = send_packet(static_cast<size_t>(nwrite));
        if (sr == SendResult::blocked) { break; }
        if (sr == SendResult::failed) { return -1; }
    }
    return flush_packets() ? 0 : -1;
}

ssize_t QuicTransport::write_stream(const uint8_t* base, size_t len) {
    size_t accepted = 0;
    while (accepted < len) {
        ngtcp2_vec datav{ const_cast<uint8_t*>(base + accepted), len - accepted };
        ngtcp2_ssize wdatalen = 0;
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi{};

        const ngtcp2_ssize nwrite =
            ngtcp2_conn_writev_stream(conn_, &ps.path, &pi,
                                      begin_packet(), QUIC_MAX_PKTLEN,
                                      &wdatalen,
                                      NGTCP2_WRITE_STREAM_FLAG_NONE,
                                      stream_id_,
                                      &datav, 1, now_ns());
        if (nwrite < 0) {
            if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
                // pump for MAX_STREAM_DATA
                if (pump_once() != 0) { errno = ECONNABORTED; return -1; }
                continue;
            }
            close_connection(static_cast<int>(nwrite));
            errno = EIO;
            return -1;
        }

        if (nwrite == 0) {
            // cwnd exhausted, not STREAM_DATA_BLOCKED
            if (pump_once() != 0) { errno = ECONNABORTED; return -1; }
            continue;
        }

        // -1 when other frames filled the packet
        if (wdatalen > 0) {
            accepted += static_cast<ssize_t>(wdatalen);
            stream_offset_ += static_cast<uint64_t>(wdatalen);
        }

        const SendResult sr = send_packet(static_cast<size_t>(nwrite));
        if (sr == SendResult::failed) { return -1; } // sendmsg left errno
        // a dropped datagram is loss NOT an error
        if (sr == SendResult::blocked && pump_once() != 0) {
            errno = ECONNABORTED;
            return -1;
        }
    }
    if (!flush_packets()) { return -1; } // errno left by the send
    return static_cast<ssize_t>(len);
}

void QuicTransport::close_connection(int liberr) {
    if (!conn_ || closed_ ||
        ngtcp2_conn_in_closing_period(conn_) ||
        ngtcp2_conn_in_draining_period(conn_)) {
        closed_ = true;
        return;
    }

    // graceful close must not orphan retained blocks
    // TCP's kernel lingers after close(); QUIC has to do it here, re-sent a lost tail
    if (liberr == 0 && !unacked_.empty()) {
        const ngtcp2_tstamp deadline = now_ns() + QUIC_CLOSE_LINER_NS;
        while (!unacked_.empty() && !closed_ && now_ns() < deadline) {
            if (pump_once(deadline) != 0) { break; }
        }
        if (closed_) { return; } // peer closed first, or the pump escalated
    }
    ngtcp2_ccerr err;
    if (liberr == 0) {
        ngtcp2_ccerr_default(&err); // app-init: NO_ERROR
    } else if (liberr == NGTCP2_ERR_CRYPTO) {
        ngtcp2_ccerr_set_tls_alert(&err, ngtcp2_conn_get_tls_alert(conn_), nullptr, 0);
    } else {
        ngtcp2_ccerr_set_liberr(&err, liberr, nullptr, 0);
    }

    // path is an out-param just like writev_stream: need backing storage
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi{};
    uint8_t* pkt = packet_buf_; // never a pool slot: send is sync

    const ngtcp2_ssize nwrite =
        ngtcp2_conn_write_connection_close(conn_, &ps.path, &pi,
                                           pkt, QUIC_MAX_PKTLEN,
                                           &err, now_ns());
    if (nwrite > 0) {
        struct iovec iov{};
        iov.iov_base = pkt;
        iov.iov_len = static_cast<size_t>(nwrite);

        struct msghdr msg{};
        msg.msg_name = nullptr; // connected socket, kernel pins the peer
        msg.msg_namelen = 0;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        // not send_packet(): the dtor cannot wait on a ring completion
        (void)::sendmsg(udp_fd_, &msg, 0);
    }

    closed_ = true;
}

// EMBR_QUIC_GRO=0 reads one datagram per recvmsg
// lazy: quic_accept_one reads the client's first datagram itself, before the socket is a connection's
void QuicTransport::init_ingress() {
    if (ingress_init_) { return; }
    ingress_init_ = true;

    const char* gro = std::getenv("EMBR_QUIC_GRO");
    const int enable = 1;
    gro_ = !(gro && std::strcmp(gro, "0") == 0) &&
           ::setsockopt(udp_fd_, SOL_UDP, UDP_GRO, &enable, sizeof(enable)) == 0;
    read_buf_.resize(gro_ ? QUIC_GRO_BUFLEN : QUIC_MAX_RECV_PKTLEN);
}

// reads until the socket is empty or QUIC_RECV_BURST packets are in: the ACKs for all of them
// are then written once, and the sender's next batch grows to match
int QuicTransport::read_packets() {
    init_ingress();

    unsigned packets = 0; // handed to ngtcp2 by this call
    for (unsigned reads = 0; reads < QUIC_RECV_BURST && packets < QUIC_RECV_BURST; ++reads) {
        sockaddr_storage remote_addr{};
        struct iovec iov{};
        iov.iov_base = read_buf_.data();
        iov.iov_len = read_buf_.size();

        alignas(cmsghdr) char cmsg_buf[CMSG_SPACE(sizeof(int))]; // room for the UDP_GRO segment size
        struct msghdr msg{};
        msg.msg_name = &remote_addr;
        msg.msg_namelen = sizeof(remote_addr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        const ssize_t nread = ::recvmsg(udp_fd_, &msg, MSG_DONTWAIT);
        if (nread < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { break; }
            // EMSGSIZE: a router's ICMP "fragmentation needed" about the size probe, not a failure
            if (errno != EMSGSIZE && errno != ECONNREFUSED &&
                errno != ENETUNREACH && errno != EHOSTUNREACH) {
                return -1;
            }
            if (errno == ECONNREFUSED) { refused_seen_ = true; }
            continue; // the report is consumed; the next read is a datagram or EAGAIN
        }
        if (msg.msg_flags & MSG_TRUNC) { return -1; } // truncated packet

        // with UDP_GRO one read can hold several datagrams of this size, the last one maybe shorter
        size_t segment_len = static_cast<size_t>(nread);
        for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_UDP && cmsg->cmsg_type == UDP_GRO &&
                cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
                int gro_segment_len = 0;
                std::memcpy(&gro_segment_len, CMSG_DATA(cmsg), sizeof(gro_segment_len));
                if (gro_segment_len > 0) { segment_len = static_cast<size_t>(gro_segment_len); }
            }
        }
        ++recv_stats_.reads;
        recv_stats_.bytes += static_cast<uint64_t>(nread);

        // path.local must equal what the factory gave conn_*_new
        // or client discards every packets "from unknown path"
        ngtcp2_path path{};
        path.local.addr = reinterpret_cast<sockaddr*>(&local_addr_);
        path.local.addrlen = local_len_;
        path.remote.addr = reinterpret_cast<sockaddr*>(&remote_addr);
        path.remote.addrlen = msg.msg_namelen;

        for (size_t offset = 0; offset < static_cast<size_t>(nread); offset += segment_len) {
            const size_t packet_len = std::min(segment_len, static_cast<size_t>(nread) - offset);
            ++recv_stats_.datagrams;
            ++packets;

            ngtcp2_pkt_info pi{};
            const int rv = feed_data(read_buf_.data() + offset, packet_len, &path, &pi);
            if (rv == NGTCP2_ERR_DRAINING || rv == NGTCP2_ERR_DROP_CONN) {
                if (rv == NGTCP2_ERR_DRAINING) {
                    // CONNECTION_CLOSE carrying NO_ERROR is the peer's orderly close
                    const ngtcp2_ccerr* cc = ngtcp2_conn_get_ccerr(conn_);
                    peer_closed_ = cc->type == NGTCP2_CCERR_TYPE_TRANSPORT &&
                                   cc->error_code == NGTCP2_NO_ERROR;
                }
                closed_ = true; // peer closed or drop: silence is the protocol
                return -1;
            }
            if (rv != 0) {
                close_connection(rv);
                return -1;
            }
        }
    }
    return static_cast<int>(packets);
}

// one read_packets -> drain_packets cycle
int QuicTransport::pump_once(ngtcp2_tstamp until) {
    if (closed_) { return -1; }

    // nothing written may sit unsent while this call waits
    if (!flush_packets()) { return -1; }

    // read first: under load the next packets are already queued and the wait is skipped
    const int packets = read_packets();
    if (packets < 0) { return -1; }

    if (packets == 0) {
        const ngtcp2_tstamp expiry = std::min(ngtcp2_conn_get_expiry(conn_), until);
        const ngtcp2_tstamp now = now_ns();

        struct timeval tv{};
        if (expiry == UINT64_MAX) {
            tv.tv_usec = 100'000; // no timer armed
        } else if (expiry > now) {
            const uint64_t diff_ns = expiry - now;
            tv.tv_sec = static_cast<time_t>(diff_ns / 1'000'000'000ULL);
            tv.tv_usec = static_cast<suseconds_t>(
                (diff_ns % 1'000'000'000ULL) / 1000ULL);
        } // already expired: tv stays {0, 0}, poll and fall through to handle_expiry

        fd_set rfds{};
        FD_ZERO(&rfds);
        FD_SET(udp_fd_, &rfds);
        const int sel = ::select(udp_fd_ + 1, &rfds, nullptr, nullptr, &tv);

        if (sel < 0 && errno != EINTR) { return -1; }
        if (sel > 0 && read_packets() < 0) { return -1; }
    }

    // a due timer and an arriving packet are not mutually exclusive
    if (ngtcp2_conn_get_expiry(conn_) <= now_ns()) {
        const int erv = ngtcp2_conn_handle_expiry(conn_, now_ns());
        if (erv == NGTCP2_ERR_IDLE_CLOSE) {
            closed_ = true; // peer presumed gone already, no need to send closing
            return -1;
        }
        if (erv == NGTCP2_ERR_HANDSHAKE_TIMEOUT) {
            handshake_timed_out_ = true;
            closed_ = true; // nobody finished connecting: nothing to tell them
            return -1;
        }
        if (erv != 0) {
            close_connection(erv);
            return -1;
        }
    }

    return drain_packets();
}

// handshake loop
int QuicTransport::run_handshake() {
    // client emits ClientHello here; needs client_initial in the factory
    if (drain_packets() != 0) { return -1; }

    while (!ngtcp2_conn_get_handshake_completed(conn_)) {
        // one read can hold the packet that completes the handshake and the peer's close after it
        if (pump_once() != 0 && !ngtcp2_conn_get_handshake_completed(conn_)) { return -1; }
    }
    return 0;
}

// --- ngtcp2 callbacks ---
int QuicTransport::on_handshake_completed(ngtcp2_conn* conn, void* user_data) {
    (void) conn; (void) user_data;
    return 0;
}

int QuicTransport::on_stream_open(ngtcp2_conn* conn,
                                  int64_t stream_id,
                                  void* user_data) {
    (void)conn;
    static_cast<QuicTransport*>(user_data)->stream_id_ = stream_id;
    return 0;
}

// ngtcp2 order: user_data (conn-level) first, stream_user_data second
int QuicTransport::on_recv_stream_data(ngtcp2_conn* conn, uint32_t flags, int64_t stream_id,
                                       uint64_t offset, const uint8_t* data, size_t datalen,
                                       void* user_data, void* stream_user_data) {
    (void)flags; (void)offset; (void) stream_user_data;
    auto* self = static_cast<QuicTransport*>(user_data);

    // recv_file waiting and nothing queued ahead: straight into the mapping
    size_t taken = 0;
    if (self->recv_dest_ && self->recv_buf_.empty()) {
        taken = std::min(datalen, self->recv_dest_len_ - self->recv_dest_got_);
        std::memcpy(self->recv_dest_ + self->recv_dest_got_, data, taken);
        self->recv_dest_got_ += taken;
    }
    if (taken < datalen) {
        self->recv_buf_.insert(self->recv_buf_.end(), data + taken, data + datalen);
    }

    // extend credit at ingest - recv_buf_ is ours
    ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
    ngtcp2_conn_extend_max_offset(conn, datalen);
    return 0;
}

void QuicTransport::on_rand(uint8_t* dest,
                            size_t destlen,
                            const ngtcp2_rand_ctx* rand_ctx) {
    (void)rand_ctx;
    (void)::getrandom(dest, destlen, 0);
}

int QuicTransport::get_new_connection_id(ngtcp2_conn* conn, ngtcp2_cid* cid, uint8_t* token,
                                         size_t cidlen, void* user_data) {
    (void)conn; (void)user_data;
    cid->datalen = cidlen;
    (void)::getrandom(cid->data, cidlen, 0);
    std::memset(token, 0, NGTCP2_STATELESS_RESET_TOKENLEN);
    return 0;
}

// EMBR_QUIC_EGRESS=sendmsg forces the copying baseline, EMBR_QUIC_GSO=0 one packet per send
// lazy: both factories connect() the socket before the first packet
void QuicTransport::init_egress() {
    if (egress_init_) { return; }
    egress_init_ = true;

    // writing 0 changes nothing; it fails only where the kernel has no UDP_SEGMENT
    const char* gso = std::getenv("EMBR_QUIC_GSO");
    const int unset = 0;
    gso_ = !(gso && std::strcmp(gso, "0") == 0) &&
           ::setsockopt(udp_fd_, SOL_UDP, UDP_SEGMENT, &unset, sizeof(unset)) == 0;
    // EMBR_QUIC_GSO_SEGS=N: fewer packets per send, for experiments
    if (const char* n = std::getenv("EMBR_QUIC_GSO_SEGS")) {
        gso_segs_ = static_cast<unsigned>(std::clamp(std::atoi(n), 1, static_cast<int>(QUIC_GSO_SEGS)));
    }
    if (gso_) { batch_buf_.resize(gso_segs_ * QUIC_MAX_PKTLEN); }

    // DF on every datagram and no kernel fragmenting: a packet too long for the path is lost or
    // fails with EMSGSIZE, so the 1350-byte probe finds out whether the path carries it
    const int df = IP_PMTUDISC_PROBE;
    ::setsockopt(udp_fd_, IPPROTO_IP, IP_MTU_DISCOVER, &df, sizeof(df));

    // EMBR_QUIC_EGRESS, for experiments: sendmsg = no SEND_ZC at all, zc = every send through SEND_ZC
    // unset, send_batch() chooses for each send
    const char* mode = std::getenv("EMBR_QUIC_EGRESS");
    if (mode && std::strcmp(mode, "sendmsg") == 0) { return; }
    zc_always_ = mode && std::strcmp(mode, "zc") == 0;
    try {
        // with GSO a slot holds a whole batch: fewer, larger slots, and the same slab whatever the batch size
        unsigned slots = gso_ ? (QUIC_ZC_GSO_PACKETS + gso_segs_ - 1) / gso_segs_ : QUIC_ZC_SLOTS;
        // EMBR_QUIC_ZC_SLOTS=N: pool size for experiments; a slot is busy until the NIC frees its packets
        if (const char* n = std::getenv("EMBR_QUIC_ZC_SLOTS")) {
            slots = static_cast<unsigned>(std::clamp(std::atoi(n), 1, 1024));
        }
        zc_ = std::make_unique<ZcEgress>(udp_fd_, (gso_ ? gso_segs_ : 1u) * QUIC_MAX_PKTLEN, slots);
        // unregistered SEND_ZC pins 8KiB per in-flight datagram against the same budget, use copy
        if (!zc_->registered()) { zc_.reset(); }
    } catch (const std::exception&) {
        zc_.reset(); // no ring: sendmsg path
    }
}

uint8_t* QuicTransport::begin_packet() {
    init_egress();
    if (batch_pkts_ > 0) { return batch_base_ + batch_len_; } // next window of the open batch
    if (zc_) {
        if (uint8_t* p = zc_->reserve()) { return batch_base_ = p; }
        zc_.reset(); // the ring latched an error -> finish this conn on sendmsg
    }
    return batch_base_ = gso_ ? batch_buf_.data() : packet_buf_;
}

// adds the packet just written at begin_packet() to the open batch
// the batch leaves when it is full or the sizes stop matching: only its last packet may be shorter
QuicTransport::SendResult QuicTransport::send_packet(size_t n) {
    ++send_stats_.datagrams;
    send_stats_.bytes += n;
    if (batch_pkts_ > 0 && n > batch_seg_) {
        // a longer packet cannot follow shorter ones: send those, start a new batch with this one
        uint8_t late[QUIC_MAX_PKTLEN];
        std::memcpy(late, batch_base_ + batch_len_, n);
        if (send_batch() == SendResult::failed) { return SendResult::failed; }
        std::memcpy(begin_packet(), late, n);
    }
    if (batch_pkts_ == 0) { batch_seg_ = n; }
    batch_len_ += n;
    ++batch_pkts_;
    if (n < batch_seg_ || batch_pkts_ >= (gso_ ? gso_segs_ : 1u)) { return send_batch(); }
    return SendResult::ok;
}

QuicTransport::SendResult QuicTransport::send_batch() {
    const size_t len = std::exchange(batch_len_, 0);
    const unsigned pkts = std::exchange(batch_pkts_, 0);
    if (pkts == 0) { return SendResult::ok; }
    ++send_stats_.sends;
    send_stats_.largest = std::max(send_stats_.largest, pkts);

    // the kernel cuts every send at the socket's UDP_SEGMENT: several packets need exactly their
    // size there, a single packet only needs to be no longer than it
    if (pkts > 1 ? sock_seg_ != batch_seg_ : (sock_seg_ != 0 && len > sock_seg_)) {
        if (zc_) { zc_->flush(); } // sends already queued were laid out for the old value
        const int seg = static_cast<int>(batch_seg_);
        if (::setsockopt(udp_fd_, SOL_UDP, UDP_SEGMENT, &seg, sizeof(seg)) < 0) {
            return SendResult::failed; // errno left for the caller's -1 contract
        }
        sock_seg_ = batch_seg_;
    }

    // SEND_ZC unless it cannot pay: a short send, or a path where the kernel copies every send anyway
    bool by_zc = zc_ != nullptr;
    if (by_zc && !zc_always_) {
        const auto& s = zc_->stats();
        zc_copied_ = zc_copied_ || (s.notifs >= QUIC_ZC_PROBE_SENDS && s.copied == s.notifs);
        by_zc = !zc_copied_ && len >= QUIC_ZC_MIN_BYTES;
    }
    if (by_zc) {
        if (zc_->commit(len)) { return SendResult::ok; }
        errno = zc_->error() != 0 ? zc_->error() : EIO; // a send CQE latched error
        return SendResult::failed;
    }
    // sendmsg copies out of the slot, which stays reserved for the next batch
    if (zc_) { zc_->flush(); } // sends queued before this one leave first: packets stay in order
    ++send_stats_.by_sendmsg;

    struct iovec iov{};
    iov.iov_base = batch_base_;
    iov.iov_len = len;

    struct msghdr msg{};
    msg.msg_name = nullptr; // connected socket, kernel pins the peer
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (::sendmsg(udp_fd_, &msg, 0) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) { return SendResult::blocked; }
        // ICMP report about an earlier datagram: this one counts as lost, same errors recv tolerates
        // EMSGSIZE: longer than the link MTU, the answer the size probe is asking for
        if (errno == ECONNREFUSED || errno == ENETUNREACH || errno == EHOSTUNREACH
            || errno == EMSGSIZE) {
            if (errno == ECONNREFUSED) { refused_seen_ = true; }
            return SendResult::blocked;
        }
        return SendResult::failed; // errno left for the caller's -1 contract
    }
    return SendResult::ok;
}

bool QuicTransport::flush_packets() {
    const bool sent = send_batch() != SendResult::failed; // a blocked batch is loss, QUIC resends
    if (!zc_) { return sent; } // sendmsg has sent each batch
    zc_->flush(); // one submit per burst
    if (zc_->refused()) { refused_seen_ = true; } // the ring took the ICMP report instead of recvmsg
    // pre-6.2 kernel: every SEND_ZC is EINVAL, stay on sendmsg
    if (zc_->unsupported()) { zc_.reset(); }
    return sent;
}

// pops every block the peer acked, one call can cover several
// offset is the prev gap-free acked offset, datalen is the new contiguous delta
int QuicTransport::on_acked_stream_data_offset(ngtcp2_conn* conn,
                                               int64_t stream_id,
                                               uint64_t offset,
                                               uint64_t datalen,
                                               void* user_data,
                                               void* stream_user_data) {
    (void)conn; (void)stream_id; (void)stream_user_data;
    auto* self = static_cast<QuicTransport*>(user_data);
    const uint64_t acked_end = offset + datalen;

    while (!self->unacked_.empty() &&
           self->unacked_.front().end_offset <= acked_end) {
        self->unacked_.pop_front(); // ~Buffer runs the erased release
    }
    return 0;
}

size_t QuicTransport::unacked_bytes() const {
    size_t total = 0;
    for (const auto& u : unacked_) { total += u.buf.size; }
    return total;
}
