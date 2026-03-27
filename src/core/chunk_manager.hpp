//
// Created by benny on 3/23/26.
//

#pragma once
#include <cstdint>
#include <vector>
#include <stdexcept>

// Manages per-chunk completion state
// v0.2: vector<bool> bitmap, sequential access
// v0.6: atomic bitmap, lock-free parallel access
class ChunkManager {
public:
    explicit ChunkManager(uint32_t count)
    : done_(count, false), count_(count) {}

    void mark_done(uint32_t idx) {
        if (idx >= count_) {
            throw std::out_of_range("ChunkManager: idx out of range");
        }
        done_[idx] = true;
    }

    bool is_done(uint32_t idx) const {
        if (idx >= count_) {
            throw std::out_of_range("ChunkManager: idx out of range");
        }
        return done_[idx];
    }

    bool all_done() const {
        for (bool d : done_) {
            if (!d) { return false; }
        }
        return true;
    }

    uint32_t count() const { return count_; }

private:
    std::vector<bool> done_;
    uint32_t count_;
};