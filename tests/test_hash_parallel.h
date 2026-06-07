//
// Created by benny on 6/6/26.
//

// tests/test_hash_parallel.cpp

#include "core/hash.hpp"       // hash_compute_parallel, sha256_buf
#include "util/constants.hpp"  // HASH_SIZE, CHUNK_SIZE

#include <fcntl.h>   // ::open, O_RDWR, O_CREAT, O_TRUNC
#include <unistd.h>  // ::close, ::unlink, ::ftruncate, ::write, ::pwrite

#include <array>     // std::array
#include <cstdint>   // uint8_t, uint32_t, uint64_t
#include <cstring>   // std::memset
#include <random>    // std::mt19937, std::uniform_int_distribution
#include <string>    // std::string
#include <vector>    // std::vector

#include <gtest/gtest.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

namespace {

struct TempFile {
    int         fd;
    std::string path;

    TempFile(int fd, std::string path) : fd(fd), path(std::move(path)) {}

    ~TempFile() {
        if (fd >= 0) { ::close(fd); }
        if (!path.empty()) { ::unlink(path.c_str()); }
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&& other) noexcept
        : fd(other.fd), path(std::move(other.path)) { other.fd = -1; }
};

static TempFile make_tempfile() {
    char tmpl[] = "/tmp/embr_hash_test_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) { throw std::runtime_error("mkstemp() failed"); }
    return {fd, std::string{tmpl}};
}

// Fill file with deterministic pseudo-random bytes, return serial hashes for comparison
static std::vector<std::array<uint8_t, HASH_SIZE>>
fill_and_hash_serial(TempFile& f, uint32_t chunk_count) {
    const uint64_t file_size = static_cast<uint64_t>(chunk_count) * CHUNK_SIZE;
    ::ftruncate(f.fd, static_cast<off_t>(file_size));

    std::mt19937 rng(42);
    std::uniform_int_distribution<uint8_t> dist(0, 255);

    std::vector<uint8_t> chunk_buf(CHUNK_SIZE);
    std::vector<std::array<uint8_t, HASH_SIZE>> serial_hashes(chunk_count);

    for (uint32_t i = 0; i < chunk_count; ++i) {
        for (auto& b : chunk_buf) { b = dist(rng); }
        ::pwrite(f.fd, chunk_buf.data(), CHUNK_SIZE,
                 static_cast<off_t>(static_cast<uint64_t>(i) * CHUNK_SIZE));
        serial_hashes[i] = sha256_buf(chunk_buf.data(), CHUNK_SIZE);
    }

    return serial_hashes;
}

// Last chunk may be smaller than CHUNK_SIZE
static std::vector<std::array<uint8_t, HASH_SIZE>>
fill_and_hash_serial_unaligned(TempFile& f,
                                uint64_t file_size,
                                uint32_t chunk_count) {
    ::ftruncate(f.fd, static_cast<off_t>(file_size));

    std::mt19937 rng(99);
    std::uniform_int_distribution<uint8_t> dist(0, 255);

    std::vector<std::array<uint8_t, HASH_SIZE>> serial_hashes(chunk_count);

    for (uint32_t i = 0; i < chunk_count; ++i) {
        const uint64_t offset    = static_cast<uint64_t>(i) * CHUNK_SIZE;
        const size_t   chunk_len = static_cast<size_t>(
            std::min(static_cast<uint64_t>(CHUNK_SIZE), file_size - offset));

        std::vector<uint8_t> buf(chunk_len);
        for (auto& b : buf) { b = dist(rng); }
        ::pwrite(f.fd, buf.data(), chunk_len, static_cast<off_t>(offset));
        serial_hashes[i] = sha256_buf(buf.data(), chunk_len);
    }

    return serial_hashes;
}

} // namespace

// ── ParallelHash — correctness ────────────────────────────────────────────────

// 4 chunks — minimal case, exercises basic worker dispatch
TEST(ParallelHash, MatchesSerial_4Chunks) {
    auto f = make_tempfile();
    const uint32_t chunk_count = 4;
    auto serial = fill_and_hash_serial(f, chunk_count);

    const uint64_t file_size = static_cast<uint64_t>(chunk_count) * CHUNK_SIZE;
    auto parallel = hash_compute_parallel(f.fd, file_size, chunk_count);

    ASSERT_EQ(parallel.size(), chunk_count);
    for (uint32_t i = 0; i < chunk_count; ++i) {
        EXPECT_EQ(parallel[i], serial[i]) << "mismatch at chunk " << i;
    }
}

// 100 chunks — exercises work-stealing across many workers
TEST(ParallelHash, MatchesSerial_100Chunks) {
    auto f = make_tempfile();
    const uint32_t chunk_count = 100;
    auto serial = fill_and_hash_serial(f, chunk_count);

    const uint64_t file_size = static_cast<uint64_t>(chunk_count) * CHUNK_SIZE;
    auto parallel = hash_compute_parallel(f.fd, file_size, chunk_count);

    ASSERT_EQ(parallel.size(), chunk_count);
    for (uint32_t i = 0; i < chunk_count; ++i) {
        EXPECT_EQ(parallel[i], serial[i]) << "mismatch at chunk " << i;
    }
}

// Single chunk — exercises the num_workers = min(chunk_count, hw_concurrency) cap
TEST(ParallelHash, MatchesSerial_SingleChunk) {
    auto f = make_tempfile();
    const uint32_t chunk_count = 1;
    auto serial = fill_and_hash_serial(f, chunk_count);

    const uint64_t file_size = static_cast<uint64_t>(chunk_count) * CHUNK_SIZE;
    auto parallel = hash_compute_parallel(f.fd, file_size, chunk_count);

    ASSERT_EQ(parallel.size(), 1u);
    EXPECT_EQ(parallel[0], serial[0]);
}

// Unaligned file — last chunk smaller than CHUNK_SIZE
TEST(ParallelHash, MatchesSerial_UnalignedLastChunk) {
    auto f = make_tempfile();
    // 3.5 chunks
    const uint64_t file_size   = 3 * CHUNK_SIZE + CHUNK_SIZE / 2;
    const uint32_t chunk_count = static_cast<uint32_t>(
        (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE);  // = 4

    auto serial = fill_and_hash_serial_unaligned(f, file_size, chunk_count);
    auto parallel = hash_compute_parallel(f.fd, file_size, chunk_count);

    ASSERT_EQ(parallel.size(), chunk_count);
    for (uint32_t i = 0; i < chunk_count; ++i) {
        EXPECT_EQ(parallel[i], serial[i]) << "mismatch at chunk " << i;
    }
}

// Sub-chunk file — file smaller than one CHUNK_SIZE
TEST(ParallelHash, MatchesSerial_SubChunkFile) {
    auto f = make_tempfile();
    const uint64_t file_size   = CHUNK_SIZE / 4;  // 256KB
    const uint32_t chunk_count = 1;

    auto serial = fill_and_hash_serial_unaligned(f, file_size, chunk_count);
    auto parallel = hash_compute_parallel(f.fd, file_size, chunk_count);

    ASSERT_EQ(parallel.size(), 1u);
    EXPECT_EQ(parallel[0], serial[0]);
}

// ── ParallelHash — determinism ────────────────────────────────────────────────

// Same file hashed twice in parallel must produce identical results
TEST(ParallelHash, Deterministic) {
    auto f = make_tempfile();
    const uint32_t chunk_count = 16;
    fill_and_hash_serial(f, chunk_count);  // fills file, discards serial result

    const uint64_t file_size = static_cast<uint64_t>(chunk_count) * CHUNK_SIZE;
    auto run1 = hash_compute_parallel(f.fd, file_size, chunk_count);
    auto run2 = hash_compute_parallel(f.fd, file_size, chunk_count);

    ASSERT_EQ(run1.size(), chunk_count);
    for (uint32_t i = 0; i < chunk_count; ++i) {
        EXPECT_EQ(run1[i], run2[i]) << "non-deterministic at chunk " << i;
    }
}

// ── ParallelHash — content sensitivity ───────────────────────────────────────

// Flipping one byte in chunk N must change only hash[N]
TEST(ParallelHash, HashChangesOnByteFlip) {
    auto f = make_tempfile();
    const uint32_t chunk_count = 4;
    fill_and_hash_serial(f, chunk_count);

    const uint64_t file_size = static_cast<uint64_t>(chunk_count) * CHUNK_SIZE;
    auto before = hash_compute_parallel(f.fd, file_size, chunk_count);

    // flip one byte in chunk 2
    uint8_t orig{};
    ::pread(f.fd, &orig, 1, static_cast<off_t>(2 * CHUNK_SIZE));
    uint8_t flipped = static_cast<uint8_t>(orig ^ 0xFF);
    ::pwrite(f.fd, &flipped, 1, static_cast<off_t>(2 * CHUNK_SIZE));

    auto after = hash_compute_parallel(f.fd, file_size, chunk_count);

    EXPECT_EQ(before[0], after[0]);  // chunk 0 unchanged
    EXPECT_EQ(before[1], after[1]);  // chunk 1 unchanged
    EXPECT_NE(before[2], after[2]);  // chunk 2 changed
    EXPECT_EQ(before[3], after[3]);  // chunk 3 unchanged
}