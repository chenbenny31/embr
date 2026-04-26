//
// Created by benny on 3/23/26.
//

#pragma once
#include "util/constants.hpp"
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// SHA256 of an in-memory buffer for per-chunk verification
std::array<uint8_t, HASH_SIZE> sha256_buf(const uint8_t* data, size_t len);

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