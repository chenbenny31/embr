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

// ── sha256_many ──────────────────────────────────────────────────────────────

// any count and any mix of sizes gives what sha256_buf gives one by one: below, at and above a
// set of lanes, with empty buffers, block-boundary sizes and a whole chunk
TEST(Sha256Many, MatchesOneByOne) {
    const std::vector<size_t> sizes = {0, 1, 55, 56, 63, 64, 65, 119, 120, 4096, 100000, CHUNK_SIZE, CHUNK_SIZE - 7};
    std::mt19937 rng(7);
    for (const size_t count : {1u, 2u, 7u, 8u, 9u, 15u, 16u, 17u, 33u, 40u}) {
        std::vector<std::vector<uint8_t>> buffers(count);
        std::vector<HashInput> inputs(count);
        for (size_t i = 0; i < count; ++i) {
            buffers[i].resize(sizes[(i * 5 + count) % sizes.size()]);
            for (auto& byte : buffers[i]) { byte = static_cast<uint8_t>(rng()); }
            inputs[i] = {buffers[i].data(), buffers[i].size()};
        }
        std::vector<std::array<uint8_t, HASH_SIZE>> digests(count);
        sha256_many(inputs, digests);
        for (size_t i = 0; i < count; ++i) {
            EXPECT_EQ(digests[i], sha256_buf(buffers[i].data(), buffers[i].size()))
                << "count " << count << " buffer " << i << " size " << buffers[i].size();
        }
    }
}

TEST(Sha256Many, CountMismatchThrows) {
    std::vector<HashInput> inputs(2);
    std::vector<std::array<uint8_t, HASH_SIZE>> digests(3);
    EXPECT_THROW(sha256_many(inputs, digests), std::runtime_error);
}

// ── hash_chunks_parallel ─────────────────────────────────────────────────────

// a list in any order, with a repeat and the short last chunk: each listed chunk's hash, in list order
TEST(ParallelHash, ListedChunksInListedOrder) {
    auto f = make_tempfile();
    const uint32_t full_chunks = 40;
    auto serial = fill_and_hash_serial(f, full_chunks);
    const std::vector<uint8_t> tail(12345, 0x5a); // a short 41st chunk
    ::pwrite(f.fd, tail.data(), tail.size(), static_cast<off_t>(static_cast<uint64_t>(full_chunks) * CHUNK_SIZE));
    serial.push_back(sha256_buf(tail.data(), tail.size()));
    const uint64_t file_size = static_cast<uint64_t>(full_chunks) * CHUNK_SIZE + tail.size();

    const std::vector<uint32_t> listed = {40, 0, 5, 39, 5, 17, 18, 19, 20, 21, 22,
                                          23, 24, 25, 26, 27, 28, 29, 30, 31, 1};
    const auto hashes = hash_chunks_parallel(f.fd, file_size, listed);
    ASSERT_EQ(hashes.size(), listed.size());
    for (size_t k = 0; k < listed.size(); ++k) {
        EXPECT_EQ(hashes[k], serial[listed[k]]) << "position " << k << " chunk " << listed[k];
    }
}

TEST(ParallelHash, NoListedChunksGivesNoHashes) {
    auto f = make_tempfile();
    EXPECT_TRUE(hash_chunks_parallel(f.fd, 0, {}).empty());
}

TEST(ParallelHash, ListedChunkPastTheEndThrows) {
    auto f = make_tempfile();
    fill_and_hash_serial(f, 2);
    const std::vector<uint32_t> listed = {0, 2};
    EXPECT_THROW(hash_chunks_parallel(f.fd, 2 * CHUNK_SIZE, listed), std::out_of_range);
}
