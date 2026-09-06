//
// Created by benny on 9/4/26.
//

#include "quic_egress.hpp"
#include <sys/socket.h>
#include <sys/uio.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

uint64_t now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
        + static_cast<uint64_t>(ts.tv_nsec);
}

}

ZcEgress::ZcEgress(int fd, size_t slot_size, unsigned slots)
    : fd_(fd), slot_size_(slot_size), slots_(slots),
      slab_(static_cast<uint64_t>(slots) * slot_size), free_(), inflight_(slots) {
    if (slots == 0 || slot_size == 0) { throw std::invalid_argument("ZcEgress: empty pool"); }

    // one CQE per send plus ont NOTIF: size the ring for both
    // a rejected SQE must not strand the rest of the burst in the SQ
    const int rv = io_uring_queue_init(slots * 2, &ring_, IORING_SETUP_SUBMIT_ALL);
    if (rv < 0) {
        throw std::runtime_error(std::string("ZcEgress: io_uring_queue_init: ")
            + std::to_string(-rv));
    }
    ring_up_ = true;

    // registration pins the slab once instead of per send
    // ENOMEM under a small memlock limit means the plain SEND_ZC path which pins per req
    iovec iov{ slab_.data(), slab_.size() };
    registered_ = (io_uring_register_buffers(&ring_, &iov, 1) == 0);

    free_.reserve(slots);
    for (unsigned i = slots; i-- > 0;) { free_.push_back(i); }
}

ZcEgress::~ZcEgress() {
    if (!ring_up_) { return; }
    const bool quiet = drain(now_ns() + 1'000'000'000ULL); // NOTIFs get one sec
    inflight_.clear(); // release the owners; no slot is handed out after
    io_uring_queue_exit(&ring_);
    // closing the ring is not completion
    if (!quiet) { (void)new std::vector<uint8_t>(std::move(slab_)); }
}

void ZcEgress::release(unsigned i) {
    free_.push_back(i);
}

uint8_t* ZcEgress::reserve() {
    if (reserved_ >= 0) { return slab_.data() + static_cast<size_t>(reserved_) * slot_size_; }
    while (free_.empty()) {
        if (err_ != 0) { return nullptr; } // ring latched an error: waiting here cannot end
        flush();
        if (free_.empty()) { reap(true); }
    }
    reserved_ = static_cast<int>(free_.back());
    free_.pop_back();
    return slab_.data() + static_cast<size_t>(reserved_) * slot_size_;
}

bool ZcEgress::commit(size_t n) {
    if (reserved_ < 0) { throw std::logic_error("ZcEgress::commit without reserve"); }
    if (n == 0 || n > slot_size_) { throw std::invalid_argument("ZcEgress::commit bad length"); }
    if (err_ != 0) { return false; }

    const unsigned i = static_cast<unsigned>(reserved_);
    uint8_t* slot = slab_.data() + static_cast<size_t>(i) * slot_size_;

    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        flush();
        sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            reap(true);
            sqe = io_uring_get_sqe(&ring_);
        }
        if (!sqe) {
            err_ = EAGAIN;
            return false;
        }
    }

    if (registered_) {
        io_uring_prep_send_zc_fixed(sqe, fd_, slot, static_cast<unsigned>(n), 0,
                                    IORING_SEND_ZC_REPORT_USAGE, 0);
    } else {
        io_uring_prep_send_zc(sqe, fd_, slot, static_cast<unsigned>(n), 0,
                              IORING_SEND_ZC_REPORT_USAGE);
    }
    io_uring_sqe_set_data64(sqe, i);

    // slot's owner: destroying the Buffer
    inflight_[i] = Buffer(slot, n, [this, i](uint8_t*) { release(i); });
    reserved_ = -1;
    ++stats_.sends;
    return true;
}

void ZcEgress::flush() {
    // submit by ring state: failed or partial submit leaves SQEs, the next flush must pick up
    while (io_uring_sq_ready(&ring_) > 0) {
        const int rv = io_uring_submit(&ring_);
        if (rv > 0) { continue; }
        if (rv < 0 && rv != -EINTR && rv != -EAGAIN && err_ == 0) { err_ = -rv; }
        break; // transient: retried on the next flush
    }
    reap(false);
}

void ZcEgress::reap(bool wait) {
    io_uring_cqe* cqe = nullptr;
    if (wait) {
        if (io_uring_sq_ready(&ring_) > 0) { flush(); }
        if (io_uring_wait_cqe(&ring_, &cqe) < 0) { return; }
    } else if (io_uring_peek_cqe(&ring_, &cqe) < 0) {
        return;
    }

    while (cqe) {
        const unsigned i = static_cast<unsigned>(io_uring_cqe_get_data64(cqe));
        const int res = cqe->res;
        const unsigned flags = cqe->flags;
        io_uring_cqe_seen(&ring_, cqe);

        if (flags & IORING_CQE_F_NOTIF) {
            ++stats_.notifs;
            if (res & IORING_NOTIF_USAGE_ZC_COPIED) { ++stats_.copied; }
            inflight_[i] = Buffer{}; // erased release -> free_.push_back(i)
        } else {
            const bool refused = res == -ENOMEM || res == -ENOBUFS || res == -EAGAIN
                              || res == -EINVAL || res == -EOPNOTSUPP;
            if (res == -EINVAL || res == -EOPNOTSUPP) { unsupported_ = true; } // no SEND_ZC kernel
            if (refused) {
                // this datagram was never sent; QUIC does a copy when loss
                ++stats_.fallback;
                if (::send(fd_, inflight_[i].ptr, inflight_[i].size, 0) < 0
                    && errno != EAGAIN && errno != ENOBUFS && errno != ENOMEM) { // transient = loss
                    ++stats_.errors;
                    if (err_ == 0) { err_ = errno; }
                }
            } else if (res < 0) {
                ++stats_.errors;
                if (err_ == 0) { err_ = -res; }
            }

            if (!(flags & IORING_CQE_F_MORE)) { // no NOTIF will follow, final for this slot
                inflight_[i] = Buffer{};
            }
        }
        if (io_uring_peek_cqe(&ring_, &cqe) < 0) { cqe = nullptr; }
    }
}

bool ZcEgress::drain(uint64_t dealine_ns) {
    flush();
    while (inflight() > 0 && now_ns() < dealine_ns) {
        __kernel_timespec ts{ 0, 50'000'000 }; // 50 ms slices to satisfy the deadline
        io_uring_cqe *cqe = nullptr;
        (void)io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
        flush(); // resubmit then reap
    }
    return inflight() == 0;
}
