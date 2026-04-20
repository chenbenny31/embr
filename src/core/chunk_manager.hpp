//
// Created by benny on 3/23/26.
//

#pragma once
#include <cstdint>
#include <vector>
#include <stdexcept>

// Manages per-chunk completion state
// v0.5: enable resume
class ChunkManager {
public:
    explicit ChunkManager(uint32_t chunk_count);

    void mark_done(uint32_t chunk_index);
    void mark_todo(uint32_t chunk_index);
    bool needs_chunk(uint32_t chunk_index) const;
    std::vector<uint32_t> needed_chunks() const;
    bool all_done() const;
    uint32_t chunk_count() const;
    const std::vector<bool>& bitmap() const;

private:
    std::vector<bool> bitmap_;
};