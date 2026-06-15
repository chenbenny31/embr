//
// Created by benny on 6/13/26.
//

#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// hand-rolled, log-spaced buckets, accurate to ~1% across 1ns-10s range
struct Histogram {
    static constexpr int NUM_BUCKETS = 512;
    static constexpr double BUCKET_BASE = 1.02; // ~1% resolution

    std::vector<uint64_t> counts;
    uint64_t total{0};
    uint64_t sum_ns{0};

    Histogram() : counts(NUM_BUCKETS, 0) {}

    void record(uint64_t value_ns) {
        int bucket = 0;
        if (value_ns > 0) {
            bucket = static_cast<int>(
                std::log(static_cast<double>(value_ns)) / std::log(BUCKET_BASE));
            if (bucket < 0) { bucket = 0; }
            if (bucket >= NUM_BUCKETS) { bucket = NUM_BUCKETS - 1; }
        }
        counts[static_cast<size_t>(bucket)]++;
        total++;
        sum_ns += value_ns;
    }

    uint64_t percentile(double p) const {
        if (total == 0) { return 0; }
        const uint64_t target = static_cast<uint64_t>(p * static_cast<double>(total));
        uint64_t cumulative = 0;
        for (int i = 0; i < NUM_BUCKETS; i++) {
            cumulative += counts[static_cast<size_t>(i)];
            if (cumulative > target) {
                return static_cast<uint64_t>(std::pow(BUCKET_BASE, static_cast<double>(i)));
            }
        }
        return static_cast<uint64_t>(std::pow(BUCKET_BASE, static_cast<double>(NUM_BUCKETS - 1)));
    }

    uint64_t p50() const { return percentile(0.50); }
    uint64_t p90() const { return percentile(0.90); }
    uint64_t p99() const { return percentile(0.99); }
    uint64_t p999() const { return percentile(0.999); }
    uint64_t mean() const { return total > 0 ? sum_ns / total : 0; }
};

struct BenchConfig {
    std::string sender_host; // sender ip
    uint16_t sender_port{10007};
    int sndbuf_bytes{0}; // 0 = autotuned baseline; >0 = pinned sweep point
    uint32_t warmup{5};
    uint32_t runs_short{1000}; // short loop: connect->first, chunk->abort
    uint32_t runs_full{50}; // full loop: complete transfer
};

// Short loop, one sample per run, raw nanoseconds, separate peer_connect and tracker_resolve
struct ShortLoopResults {
    std::vector<uint64_t> peer_connect_ns; // tcp_connect wall time only
    std::vector<uint64_t> ttfc_ns; // CHUNK_HDR first byte
    uint64_t file_size{0};
    uint32_t chunk_count{0};
};

// Full loop, completion raw samples, hash-verify via histogram
struct FullLoopResults {
    std::vector<uint64_t> completion_ns; // full transfer wall time per run
    Histogram verify_ns; // per-chunk hash-verify, all runs combined
    uint64_t file_size{0};
    uint32_t chunk_count{0};
};

ShortLoopResults run_short_loop(const BenchConfig& cfg);
FullLoopResults run_full_loop(const BenchConfig& cfg);

void print_short_results(const ShortLoopResults& results, uint32_t runs);
void print_full_results(const FullLoopResults& results, uint32_t runs);