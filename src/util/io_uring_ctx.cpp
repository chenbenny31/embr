//
// Created by benny on 4/4/26.
//

#include "io_uring_ctx.hpp"

#include <sys/mman.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "constants.hpp"
#include <liburing.h>

IoUringCtx::IoUringCtx(size_t chunk_buf_count, size_t chunk_buf_size,
                       size_t frag_buf_count, size_t frag_buf_size)
                           : chunk_buf_count_(chunk_buf_count)
                           , chunk_buf_size_(chunk_buf_size)
                           , frag_buf_count_(frag_buf_count)
                           , frag_buf_size_(frag_buf_size) {
    // init io_uring

    // SQ=256: sufficient for recv pipeline, bounded by UDP_FRAG_BUFS=64 in-flight ops
    // CQ=256 * 2: sufficient for concurrent RECV+WRITE CQEs
    constexpr unsigned QUEUE_DEPTH = 256;

    int init_ret = io_uring_queue_init(QUEUE_DEPTH, &ring_, 0);
    if (init_ret < 0) {
        throw std::runtime_error("IoUringCtx: io_uring_queue_init failed: " +
                                 std::string(std::strerror(-init_ret)));
    }

    // allocate buf backing mem (page-aligned via mmap)
    size_t chunk_total = chunk_buf_count_ * chunk_buf_size_;
    size_t frag_total = frag_buf_count_ * frag_buf_size_;

    chunk_mem_ = static_cast<uint8_t*>(
        ::mmap(nullptr, chunk_total,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (chunk_mem_ == MAP_FAILED) {
        io_uring_queue_exit(&ring_);
        throw std::runtime_error("IoUringCtx: mmap chunk_mem failed: " +
             std::string(std::strerror(errno)));
    }

    frag_mem_ = static_cast<uint8_t*>(
        ::mmap(nullptr, frag_total,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (frag_mem_ == MAP_FAILED) {
        ::munmap(chunk_mem_, chunk_total);
        io_uring_queue_exit(&ring_);
        throw std::runtime_error("IoUringCtx: mmap frag_mem failed: " +
            std::string(std::strerror(errno)));
    }

    // register all bufs in one call
    // iovec layout: chunk bufs[0..chunk_buf_count_-1], frag bufs[chunk_buf_count..]
    std::vector<iovec> iovecs(chunk_buf_count_ + frag_buf_count_);

    for (size_t i = 0; i < chunk_buf_count_; ++i) {
        iovecs[i].iov_base = chunk_mem_ + i * chunk_buf_size_;
        iovecs[i].iov_len = chunk_buf_size_;
    }
    for (size_t i = 0; i < frag_buf_count_; ++i) {
        iovecs[chunk_buf_count_ + i].iov_base = frag_mem_ + i * frag_buf_size_;
        iovecs[chunk_buf_count_ + i].iov_len = frag_buf_size_;
    }

    int ret = io_uring_register_buffers(&ring_,
                                  iovecs.data(), static_cast<unsigned>(iovecs.size()));
    if (ret < 0) {
        if (-ret == ENOMEM) {
            // memlock limit exceeded - fall back to unregistered buffers
            // data plane will use prep_read_prep_write instead of _fixed variants
            std::cerr << "[io_uring] memlock limit exceeded, falling back to "
                         "unregistered buffers (zero-copy disabled)\n";
        } else {
            ::munmap(frag_mem_, frag_total);
            ::munmap(chunk_mem_, chunk_total);
            io_uring_queue_exit(&ring_);
            throw std::runtime_error("IoUringCtx: register_buffers failed: " +
                                     std::string(std::strerror(-ret)));
        }
    } else {
        registered_buffers_ = true;
    }
}

IoUringCtx::~IoUringCtx() {
    if (registered_buffers_) {
        io_uring_unregister_buffers(&ring_);
    }
    io_uring_queue_exit(&ring_);
    ::munmap(frag_mem_, frag_buf_count_ * frag_buf_size_);
    ::munmap(chunk_mem_, chunk_buf_count_ * chunk_buf_size_);
}

uint8_t* IoUringCtx::chunk_buf(size_t idx) {
    if (idx >= chunk_buf_count_) {
        throw std::runtime_error("IoUringCtx::chunk_buf index out of range");
    }
    return chunk_mem_ + idx * chunk_buf_size_;
}

uint8_t* IoUringCtx::frag_buf(size_t idx) {
    if (idx >= frag_buf_count_) {
        throw std::runtime_error("IoUringCtx::frag_buf index out of range");
    }
    return frag_mem_ + idx * frag_buf_size_;
}

io_uring* IoUringCtx::ring() {
    return &ring_;
}
