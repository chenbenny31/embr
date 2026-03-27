//
// Created by benny on 3/23/26.
//

#pragma once
#include <cstddef>

// I/O buf size fits L2 cache
inline constexpr size_t READ_BUF_SIZE = 1024 * 1024; // 1MB

// Chunk size
// v0.2: protocol chunking boundary
// v0.6: parallel in-flight chunk unit
inline constexpr size_t CHUNK_SIZE = 16 * 1024 * 1024; // 16MB