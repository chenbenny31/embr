//
// Created by benny on 3/23/26.
//

#pragma once
#include "util/constants.hpp"
#include <span>
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// SHA256 of an in-memory buffer for per-chunk verification
std::array<uint8_t, HASH_SIZE> sha256_buf(const uint8_t* data, size_t len);

// one buffer of a sha256_many call
struct HashInput {
    const uint8_t* data;
    size_t len;
};

// SHA256 of several buffers. With ISA-L crypto built in and AVX-512 (or AVX2 without SHA
// extensions) on the CPU, buffers are hashed side by side in SIMD lanes; for fewer than half a
// set of lanes, and everywhere else, one by one as sha256_buf does.
void sha256_many(std::span<const HashInput> inputs,
                 std::span<std::array<uint8_t, HASH_SIZE>> digests);

// buffers worth collecting for one sha256_many call on this CPU; 1 when one by one is as fast.
// EMBR_HASH_LANES=N overrides it (1 turns the side-by-side path off)
unsigned sha256_lanes();

// Returns ".{file_name}.embr.hash" in the same directory as file_path
std::string hash_cache_path_for(const std::string& file_path);

// Loads chunk hashes from cache if (file_size, mtime_ns) both match
bool hash_cache_load(const std::string& file_path,
                     uint64_t file_size,
                     uint64_t mtime_ns,
                     uint32_t chunk_count,
                     std::vector<std::array<uint8_t, HASH_SIZE>>& chunk_hashes);

// Writes chunk hashes to cache alongside file_path, overwrites if exists
void hash_cache_save(const std::string& file_path,
                     uint64_t file_size,
                     uint64_t mtime_ns,
                     uint32_t chunk_size,
                     uint32_t chunk_count,
                     const std::vector<std::array<uint8_t, HASH_SIZE>>& chunk_hashes);

// Compute SHA256 for chunks in parallel, return hashes in chunk order
// mmap the entire file once, min(hardware_concurrency, chunk_count) jthread workers
// with alignas(64) atomic counter
std::vector<std::array<uint8_t, HASH_SIZE>> hash_compute_parallel(int file_fd,
                                                                  uint64_t file_size,
                                                                  uint32_t chunk_count);