//
// Created by benny on 9/4/26.
//

// io_uring SEND_ZC egress for one connected UDP socket
// one packet slot is handed to ngtcp2 as assembly dest, commit as IORING_OP_SEND_ZC
// owned by Buffer erased release returns the slot to free list
// kernel may read the slot until the IORING_CQE_F_NOTIF completion: pool lives beside unacked_
//
// Completion rule: send CQE carries IORING_CQE_F_MORE when NOTIF will follow: keep slot
// a send CQE with F_MORe is final and slot releases on it

#pragma once

#include "core/protocol.hpp"
#include <liburing.h>
#include <cstddef>
#include <cstdint>
#include <vector>

class ZcEgress {
public:
    // fd must be a connected UDP sockets; throw if the ring cannot be created
    ZcEgress(int fd, size_t slot_size, unsigned slots);
    ~ZcEgress(); // drains outstanding sends (bounded), then exits the ring

    ZcEgress(const ZcEgress&) = delete;
    ZcEgress& operator=(const ZcEgress&) = delete;

    // assembly dest for the next datagram; the same slot is returned again until commit() takes it
    // nullptr once the ring has latched an error: the caller should stop using the egress
    uint8_t* reserve();
    // enqueue the reserved slot as one SEND_ZC of n bytes; false on a latched error
    bool commit(size_t n);
    // submit queued SQEs and recycle any completed slots without blocking
    void flush();
    // wait until every committed slot been released or pass the DDL, return true
    bool drain(uint64_t dealine_ns);

    size_t slot_size() const { return slot_size_; }
    unsigned inflight() const {
        return slots_ - static_cast<unsigned>(free_.size()) - (reserved_ >= 0 ? 1u : 0u);
    }
    bool registered() { return registered_; }
    bool unsupported() const { return unsupported_; } // kernel reject SEND_ZC
    int error() const { return err_; } // first negative send result as errno; 0 if none

    struct Stats {
        uint64_t sends{0};
        uint64_t notifs{0};
        uint64_t copied{0};
        uint64_t fallback{0}; // copies attempted after a refused SEND_ZC; failed copy also counts
        uint64_t errors{0};
    };
    const Stats& stats() const { return stats_;}

private:
    void reap(bool wait); // process CQEs; blocks for least one if wait
    void release(unsigned i);

    int fd_;
    size_t slot_size_;
    unsigned slots_;
    io_uring ring_{};
    bool ring_up_{false};
    bool registered_{false};
    bool unsupported_{false};

    // destruction order: inflight_ hold Buffers (their release touches free_)
    std::vector<uint8_t> slab_;
    std::vector<unsigned> free_;
    std::vector<Buffer> inflight_; // declared last -> destroy first

    int reserved_{-1};
    int err_{0};
    Stats stats_;
};
