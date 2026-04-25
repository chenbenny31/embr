//
// Created by benny on 4/19/26.
//

#include "chunk_manager.hpp"
#include <stdexcept>

ChunkManager::ChunkManager(uint32_t chunk_count)
    : bitmap_(chunk_count, false) {}

void ChunkManager::mark_done(uint32_t chunk_index) {
    if (chunk_index >= bitmap_.size()) {
        throw std::out_of_range("ChunkManager::mark_done: Chunk index out of range");
    }
    bitmap_[chunk_index] = true;
}

void ChunkManager::mark_todo(uint32_t chunk_index) {
    if (chunk_index >= bitmap_.size()) {
        throw std::out_of_range("ChunkManager::mark_todo: Chunk index out of range");
    }
    bitmap_[chunk_index] = false;
}

bool ChunkManager::needs_chunk(uint32_t chunk_index) const {
    if (chunk_index >= bitmap_.size()) {
        throw std::out_of_range("ChunkManager::needs_chunk: Chunk index out of range");
    }
    return bitmap_[chunk_index] == false;
}

std::vector<uint32_t> ChunkManager::needed_chunks() const {
    std::vector<uint32_t> result;
    for (uint32_t i = 0; i < static_cast<uint32_t>(bitmap_.size()); ++i) {
        if (!bitmap_[i]) {
            result.push_back(i);
        }
    }
    return result;
}

bool ChunkManager::all_done() const {
    for (bool is_done : bitmap_) {
        if (!is_done) { return false;}
    }
    return true;
}

uint32_t ChunkManager::chunk_count() const {
    return static_cast<uint32_t>(bitmap_.size());
}

const std::vector<bool>& ChunkManager::bitmap() const {
    return bitmap_;
}