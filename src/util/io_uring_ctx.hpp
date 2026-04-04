//
// Created by benny on 4/4/26.
//

#pragma once
#include <cstddef>
#include <cstdint>
#include <liburing.h>

// Owns the io_uring instance and two registered buffer pools:
// - chunk buffers - CHUNK_SIZE each, for READ_FIXED + sendmsg iovec (send side)
// - frag buffers - UDP_MTU each, for RECV_FIXED + WRITE_FIXED ( recv side)
// All buffers registered in one io_uring_register_buffers() call
// No copy and move: io_uring kernel state is bound to specific fd and registered buf addr
class IoUringCtx {
public:
    IoUringCtx(size_t chunk_buf_count, size_t chunk_buf_size,
               size_t frag_buf_count, size_t frag_buf_size);
    ~IoUringCtx();

    // disable copy and move
    IoUringCtx(const IoUringCtx&) = delete;
    IoUringCtx& operator=(const IoUringCtx&) = delete;
    IoUringCtx(IoUringCtx&&) = delete;
    IoUringCtx& operator=(IoUringCtx&&) = delete;

    // registered buf access
    uint8_t* chunk_buf(size_t idx); // idx < chunk_buf_count
    uint8_t* frag_buf(size_t idx); // idx < frag_buf_count
    io_uring* ring();

    size_t chunk_buf_count() const { return chunk_buf_count_; }
    size_t chunk_buf_size() const { return chunk_buf_size_; }
    size_t frag_buf_count() const { return frag_buf_count_; }
    size_t frag_buf_size() const { return frag_buf_size_; }
    size_t ring_index_chunk(size_t idx) const { return idx; }
    size_t ring_index_frag(size_t idx) const { return chunk_buf_count_ + idx; }

private:
    io_uring ring_{};

    // contiguous backing mem - chunk buf first then frag buf
    // registered as one iovec array in io_uring_register_buffers()
    uint8_t* chunk_mem_{nullptr};
    uint8_t* frag_mem_{nullptr};

    size_t chunk_buf_count_;
    size_t chunk_buf_size_;
    size_t frag_buf_count_;
    size_t frag_buf_size_;
};