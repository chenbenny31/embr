//
// Created by benny on 4/19/26.
//

#pragma once

#include "chunk_manager.hpp"
#include "util/constants.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>

class PartialFile {
public:
    // Returns ".{filename}.embr.partial" in the same dir as output_path
    static std::string path_for(const std::string& output_path);

    // Loads a partial file and returns a ChunkManager reflecting prior progress
    // Returns nullopt if no partial file exists (fresh) or file_hash/chunk_count mismatch
    static std::optional<ChunkManager> load(
        const std::string& output_path,
        const std::array<uint8_t, HASH_SIZE>& file_hash,
        uint32_t chunk_count);

    // Persists current bitmap to .embr.partial alongside output_path
    static void save(
        const std::string& output_path,
        const std::array<uint8_t, HASH_SIZE>& file_hash,
        const ChunkManager& chunk_manager);

    // Remove the .embr.partial file, Been called after transfer success
    static void remove(const std::string& output_path);
};