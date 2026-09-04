//
// Created by benny on 6/22/26.
//

#include "quic_transport.hpp"
#include <arpa/inet.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <ctime>
#include <unistd.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <algorithm>

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

QuicTransport::~QuicTransport() {
    if (conn_ && !closed_) { close_connection(0); } // graceful NO_ERROR
    if (conn_) { ngtcp2_conn_del(conn_); }
    if (ssl_) { wolfSSL_free(ssl_); }
    if (ssl_ctx_) { wolfSSL_CTX_free(ssl_ctx_); }
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
    while (recv_buf_.empty() && !stream_fin_received_ && !closed_) {
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

    if (stream_fin_received_) { return 0; } // clean EOF, matches TCP recv()==0
    return -1;
}

int QuicTransport::send_fin() {
    if (stream_id_ < 0) { return -1; }
    if (fin_sent_) { return 0; } // FIN is a state

    for (;;) {
        if (closed_) { return -1; }

        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi{};
        ngtcp2_ssize wdatalen = -1;

        // empty data + FIN flag serializes a 0-length STREAM+fin and sets wdatalen 0
        const ngtcp2_ssize nwrite =
            ngtcp2_conn_writev_stream(conn_, &ps.path, &pi,
                                      begin_packet(), QUIC_MAX_PKTLEN,
                                      &wdatalen,
                                      NGTCP2_WRITE_STREAM_FLAG_FIN,
                                      stream_id_,
                                      nullptr, 0,
                                      now_ns());

        if (nwrite < 0) {
            if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
                if (pump_once() != 0) { return -1; }
                continue;
            }
            close_connection(static_cast<int>(nwrite));
            return -1;
        }

        if (nwrite == 0) { // cwnd or pacing, let ACKs in and retry
            if (pump_once() != 0) { return -1; }
            continue;
        }

        if (send_packet(static_cast<size_t>(nwrite)) == SendResult::failed) {
            return -1; // blocked is tolerated, FIN retransmits
        }

        if (wdatalen >= 0) {
            fin_sent_ = true;
            flush_packets();
            return 0;
        }
    }
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
        if (stream_fin_received_ || closed_) {
            throw std::runtime_error("QuicTransport::recv_file: connection closed");
        }
        if (pump_once() != 0) {
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
    flush_packets();
    return 0;
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
            if (pump_once() != 0) { ; return -1; }
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
    flush_packets();
    return static_cast<ssize_t>(len);
}

void QuicTransport::close_connection(int liberr) {
    if (!conn_ || closed_ ||
        ngtcp2_conn_in_closing_period(conn_) ||
        ngtcp2_conn_in_draining_period(conn_)) {
        closed_ = true;
        return;
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
    uint8_t* pkt = begin_packet();

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

// one recvmsg -> feed_data -> drain_packets cycle
int QuicTransport::pump_once() {
    if (closed_) { return -1; }

    const ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(conn_);
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

    if (sel > 0) {
        // recv cap is independent of send cap
        uint8_t buf[QUIC_MAX_RECV_PKTLEN];
        sockaddr_storage remote_addr{};
        struct iovec iov{};
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);

        struct msghdr msg{};
        msg.msg_name = &remote_addr;
        msg.msg_namelen = sizeof(remote_addr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        const ssize_t nread = ::recvmsg(udp_fd_, &msg, MSG_DONTWAIT);
        if (nread < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK &&
                errno != ECONNREFUSED && errno != ENETUNREACH && errno != EHOSTUNREACH) {
                return -1;
            }
        } else if (msg.msg_flags & MSG_TRUNC) {
            return -1; // truncated packet
        } else {
            // path.local must equal what the factory gave conn_*_new
            // or client discards every packets "from unknown path"
            ngtcp2_path path{};
            path.local.addr = reinterpret_cast<sockaddr*>(&local_addr_);
            path.local.addrlen = local_len_;
            path.remote.addr = reinterpret_cast<sockaddr*>(&remote_addr);
            path.remote.addrlen = msg.msg_namelen;

            ngtcp2_pkt_info pi{};
            const int rv = feed_data(buf, static_cast<size_t>(nread), &path, &pi);
            if (rv == NGTCP2_ERR_DRAINING || rv == NGTCP2_ERR_DROP_CONN) {
                closed_ = true; // peer closed or drop: silence is the protocol
                return -1;
            }
            if (rv != 0) {
                close_connection(rv);
                return -1;
            }
        }
    }

    // a due timer and an arriving packet are not mutually exclusive
    if (ngtcp2_conn_get_expiry(conn_) <= now_ns()) {
        const int erv = ngtcp2_conn_handle_expiry(conn_, now_ns());
        if (erv == NGTCP2_ERR_IDLE_CLOSE) {
            closed_ = true; // peer presumed gone already, no need to send closing
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
        if (pump_once() != 0) { return -1; }
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
    (void)offset; (void) stream_user_data;
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

    if (flags & NGTCP2_STREAM_DATA_FLAG_FIN) {
        self->stream_fin_received_ = true;
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

uint8_t* QuicTransport::begin_packet() {
    return packet_buf_; // pool slot in ring modes
}

QuicTransport::SendResult QuicTransport::send_packet(size_t n) {
    struct iovec iov{};
    iov.iov_base = packet_buf_;
    iov.iov_len = n;

    struct msghdr msg{};
    msg.msg_name = nullptr; // connected socket, kernel pins the peer
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (::sendmsg(udp_fd_, &msg, 0) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) { return SendResult::blocked; }
        return SendResult::failed; // errno left for the caller's -1 contract
    }
    return SendResult::ok;
}

void QuicTransport::flush_packets() {} // sendmsg submits per datagram, the ring batches here

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
