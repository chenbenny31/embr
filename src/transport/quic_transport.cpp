//
// Created by benny on 6/22/26.
//

#include "quic_transport.hpp"
#include <arpa/inet.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
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

QuicTransport::QuicTransport(int udp_fd, ngtcp2_conn* conn, WOLFSSL* ssl, WOLFSSL_CTX* ssl_ctx)
        : udp_fd_(udp_fd), conn_(conn), ssl_(ssl), ssl_ctx_(ssl_ctx) {

    // crypto_conn_ref_ links ngtcp2 crypto callbacks
    crypto_conn_ref_.get_conn = [](ngtcp2_crypto_conn_ref* ref) -> ngtcp2_conn* {
       return static_cast<QuicTransport*>(ref->user_data)->conn_;
    };
    crypto_conn_ref_.user_data = this;
    // attach conn_ref to SSL session
    wolfSSL_set_app_data(ssl_, &crypto_conn_ref_);
    // tell ngtcp2 which SSL session handles TLS for this conn
    ngtcp2_conn_set_tls_native_handle(conn_, ssl_);
}

QuicTransport::~QuicTransport() {
    if (conn_) { ngtcp2_conn_del(conn_); }
    if (ssl_) { wolfSSL_free(ssl_); }
    if (ssl_ctx_) { wolfSSL_CTX_free(ssl_ctx_); }
    if (udp_fd_ >= 0) { ::close(udp_fd_); }
}

// --- control plane ---
ssize_t QuicTransport::send(const uint8_t* buf, size_t len) {
    if (stream_id_ < 0) {
        throw std::runtime_error("QuicTransport::send: no open stream");
    }

    sockaddr_storage local_addr{};
    socklen_t local_len = sizeof(local_addr);
    ::getsockname(udp_fd_, reinterpret_cast<sockaddr*>(&local_addr), &local_len);

    const uint8_t* ptr = buf;
    size_t remain = len;

    while (remain > 0) {
        ngtcp2_vec datav{ const_cast<uint8_t*>(ptr), remain };
        ngtcp2_ssize wdatalen = 0;
        uint8_t pkt[QUIC_MAX_PKTLEN];
        ngtcp2_path path{};
        ngtcp2_pkt_info pi{};

        const ngtcp2_ssize nwrite =
            ngtcp2_conn_writev_stream(conn_, &path, &pi,
                                      pkt, sizeof(pkt),
                                      &wdatalen,
                                      NGTCP2_WRITE_STREAM_FLAG_NONE,
                                      stream_id_,
                                      &datav, 1,
                                      now_ns());

        if (nwrite < 0) {
            if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
                if (pump_once(local_addr, local_len) != 0) { return -1; }
                continue;
            }
            throw std::runtime_error(
                std::string("QuicTransport::send: failed to write to stream ") +
                ngtcp2_strerror(static_cast<int>(nwrite)));
        }

        if (nwrite > 0) {
            // send assembled + encrypted packet
            struct iovec iov{};
            iov.iov_base = pkt;
            iov.iov_len = static_cast<size_t>(nwrite);

            struct msghdr msg{};
            msg.msg_name = path.remote.addr;
            msg.msg_namelen = path.remote.addrlen;
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;

            if (::sendmsg(udp_fd_, &msg, 0) < 0) {
                throw std::runtime_error(
                    std::string("QuicTransport::send: sendmsg failed: ") +
                    std::strerror(errno));
            }
        }

        if (wdatalen > 0) {
            ptr += static_cast<size_t>(wdatalen);
            remain -= static_cast<size_t>(wdatalen);
        }
    }

    return static_cast<ssize_t>(len);
}

ssize_t QuicTransport::recv(uint8_t* buf, size_t len) {
    if (stream_id_ < 0) {
        throw std::runtime_error("QuicTransport::recv: no open stream");
    }

    sockaddr_storage local_addr{};
    socklen_t local_len = sizeof(local_addr);
    ::getsockname(udp_fd_, reinterpret_cast<sockaddr*>(&local_addr), &local_len);

    // block until recv_buf_ has data, pump I/O to make progress
    while (recv_buf_.empty()) {
        if (pump_once(local_addr, local_len) != 0) { return -1; }
    }

    const size_t n = std::min(len, recv_buf_.size());
    std::memcpy(buf, recv_buf_.data(), n);
    recv_buf_.erase(recv_buf_.begin(),
                    recv_buf_.begin() + static_cast<ptrdiff_t>(n));
    return static_cast<ssize_t>(n);
}

// --- data plane ---
void QuicTransport::send_file(int file_fd, uint64_t offset, size_t len) {
    (void)file_fd; (void)offset; (void)len;
    throw std::runtime_error("QuicTransport::send_file: not implemented");
}

void QuicTransport::recv_file(int file_fd, uint64_t offset, size_t len) {
    (void)file_fd; (void)offset; (void)len;
    throw std::runtime_error("QuicTransport::recv_file: not implemented");
}

// --- I/O helpers ---
int QuicTransport::feed_data(const uint8_t* data,
                             size_t datalen,
                             const ngtcp2_path* path,
                             const ngtcp2_pkt_info* pi) {
    return ngtcp2_conn_read_pkt(conn_, path, pi, data, datalen, now_ns());
}

int QuicTransport::drain_packets() {
    uint8_t buf[QUIC_MAX_PKTLEN];

    for (size_t i = 0; i < QUIC_MAX_BURST; i++) {
        ngtcp2_path path{};
        ngtcp2_pkt_info pi{};
        ngtcp2_ssize wdatalen = 0;

        const ngtcp2_ssize nwrite =
            ngtcp2_conn_writev_stream(conn_, &path, &pi,
                                      buf, sizeof(buf),
                                      &wdatalen,
                                      NGTCP2_WRITE_STREAM_FLAG_NONE,
                                      stream_id_,
                                      nullptr, 0,
                                      now_ns());
        if (nwrite == 0) { break; }
        if (nwrite < 0) {
            if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED ||
                nwrite == NGTCP2_ERR_STREAM_SHUT_WR) {
                break;
            }
            return static_cast<int>(nwrite);
        }

        struct iovec iov{};
        iov.iov_base = buf;
        iov.iov_len = static_cast<size_t>(nwrite);

        struct msghdr msg{};
        msg.msg_name = path.remote.addr;
        msg.msg_namelen = path.remote.addrlen;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        if (::sendmsg(udp_fd_, &msg, 0) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { break; }
            return -1;
        }
    }
    return 0;
}

// one recvmsg -> feed_data -> drain_packets cycle
int QuicTransport::pump_once(const sockaddr_storage& local_addr,
                             socklen_t local_len) {
    const ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(conn_);
    const ngtcp2_tstamp now = now_ns();

    struct timeval tv{};
    if (expiry != UINT64_MAX && expiry > now) {
        const uint64_t diff_ns = expiry - now;
        tv.tv_sec = static_cast<time_t>(diff_ns / 1'000'000'000ULL);
        tv.tv_usec = static_cast<suseconds_t>(
            (diff_ns % 1'000'000'000ULL) / 1000ULL);
    } else {
        tv.tv_sec = 0;
        tv.tv_usec = 100'000; // 100ms fallback
    }

    fd_set rfds{};
    FD_ZERO(&rfds);
    FD_SET(udp_fd_, &rfds);
    const int sel = ::select(udp_fd_ + 1, &rfds, nullptr, nullptr, &tv);

    if (sel < 0) {
        if (errno == EINTR) { return 0; }
        return -1;
    }

    if (sel == 0) {
        // timer fired
        if (ngtcp2_conn_handle_expiry(conn_, now_ns()) != 0) { return -1; }
    } else {
        uint8_t buf[QUIC_MAX_PKTLEN];
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) { return 0; }
            return -1;
        }

        ngtcp2_path path{};
        path.local.addr = reinterpret_cast<sockaddr*>(
            const_cast<sockaddr_storage*>(&local_addr));
        path.local.addrlen = local_len;
        path.remote.addr = reinterpret_cast<sockaddr*>(&remote_addr);
        path.remote.addrlen = msg.msg_namelen;

        ngtcp2_pkt_info pi{};
        if (feed_data(buf, static_cast<size_t>(nread), &path, &pi) != 0) {
            return -1;
        }
    }
    return drain_packets();
}

// handshake loop
int QuicTransport::run_handshake() {
    sockaddr_storage local_addr{};
    socklen_t local_len = sizeof(local_addr);
    ::getsockname(udp_fd_, reinterpret_cast<sockaddr*>(&local_addr), &local_len);

    if (drain_packets() != 0) { return -1; }

    while (!ngtcp2_conn_get_handshake_completed(conn_)) {
        if (pump_once(local_addr, local_len) != 0) { return -1; }
    }
    return 0;
}

// --- ngtcp2 callbacks ---
int QuicTransport::on_recv_crypto_data(ngtcp2_conn* conn, ngtcp2_encryption_level level,
                                       uint64_t offset, const uint8_t* data, size_t datalen,
                                       void* user_data) {
    return ngtcp2_crypto_recv_crypto_data_cb(conn, level, offset,
                                             data, datalen, user_data);
}

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
    (void)conn; (void)flags; (void)stream_id; (void)offset; (void) stream_user_data;
    auto* self = static_cast<QuicTransport*>(user_data);
    self->recv_buf_.insert(self->recv_buf_.end(), data, data + datalen);
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