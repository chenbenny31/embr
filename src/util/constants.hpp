//
// Created by benny on 3/23/26.
//

#pragma once
#include <cstddef>

// Chunk
inline constexpr size_t READ_BUF_SIZE = 1 * 1024 * 1024; // TCP-path fallback, L2 cache fit
inline constexpr size_t CHUNK_SIZE = 1 * 1024 * 1024; // v0.5 adapt: 16MB raise memlock

// UDP / io_uring
// UDP fragment
inline constexpr size_t UDP_MTU = 65000; // for localhost benchmark only
inline constexpr size_t FRAG_HDR_SIZE = 10;
inline constexpr size_t UDP_PAYLOAD_SIZE = UDP_MTU - FRAG_HDR_SIZE;
inline constexpr size_t MAX_FRAGS_PER_CHUNK =
    (CHUNK_SIZE + UDP_PAYLOAD_SIZE - 1) / UDP_PAYLOAD_SIZE;

// io_uring buffer pool
inline constexpr size_t UDP_CHUNK_BUFS = 2; // 2 * 1MB, double buffer send pipeline
inline constexpr size_t UDP_FRAG_BUFS = 32; // 32 * 65KB = 2MB