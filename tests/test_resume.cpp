//
// Created by benny on 4/20/26.
//

// tests/test_resume.cpp

#include "core/chunk_manager.hpp"
#include "core/partial_file.hpp"
#include "core/hash.hpp"
#include "util/constants.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <gtest/gtest.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

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
    char tmpl[] = "/tmp/embr_resume_test_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) { throw std::runtime_error("mkstemp() failed"); }
    return {fd, std::string{tmpl}};
}

static std::array<uint8_t, HASH_SIZE> make_fake_hash(uint8_t seed) {
    std::array<uint8_t, HASH_SIZE> hash{};
    std::memset(hash.data(), seed, HASH_SIZE);
    return hash;
}

// ── ChunkManager ─────────────────────────────────────────────────────────────

TEST(ChunkManager, FreshAllNeeded) {
    ChunkManager cm(5);
    EXPECT_EQ(cm.chunk_count(), 5u);
    EXPECT_FALSE(cm.all_done());
    auto needed = cm.needed_chunks();
    ASSERT_EQ(needed.size(), 5u);
    for (uint32_t i = 0; i < 5; ++i) {
        EXPECT_EQ(needed[i], i);
    }
}

TEST(ChunkManager, MarkDoneReducesNeeded) {
    ChunkManager cm(4);
    cm.mark_done(0);
    cm.mark_done(2);
    auto needed = cm.needed_chunks();
    ASSERT_EQ(needed.size(), 2u);
    EXPECT_EQ(needed[0], 1u);
    EXPECT_EQ(needed[1], 3u);
}

TEST(ChunkManager, MarkTodoRestoresNeeded) {
    ChunkManager cm(3);
    cm.mark_done(0);
    cm.mark_done(1);
    cm.mark_done(2);
    EXPECT_TRUE(cm.all_done());
    cm.mark_todo(1);
    EXPECT_FALSE(cm.all_done());
    auto needed = cm.needed_chunks();
    ASSERT_EQ(needed.size(), 1u);
    EXPECT_EQ(needed[0], 1u);
}

TEST(ChunkManager, AllDoneWhenAllMarked) {
    ChunkManager cm(3);
    cm.mark_done(0);
    cm.mark_done(1);
    cm.mark_done(2);
    EXPECT_TRUE(cm.all_done());
    EXPECT_TRUE(cm.needed_chunks().empty());
}

TEST(ChunkManager, OutOfRangeThrows) {
    ChunkManager cm(3);
    EXPECT_THROW(cm.mark_done(3), std::out_of_range);
    EXPECT_THROW(cm.mark_todo(3), std::out_of_range);
    EXPECT_THROW(cm.needs_chunk(3), std::out_of_range);
}

// ── PartialFile ───────────────────────────────────────────────────────────────

TEST(PartialFile, PathFor_NoDirectory) {
    EXPECT_EQ(PartialFile::path_for("file.bin"), ".file.bin.embr.partial");
}

TEST(PartialFile, PathFor_WithDirectory) {
    EXPECT_EQ(PartialFile::path_for("/tmp/file.bin"), "/tmp/.file.bin.embr.partial");
}

TEST(PartialFile, LoadReturnsNulloptIfNoFile) {
    auto hash = make_fake_hash(0xAB);
    auto result = PartialFile::load("/tmp/embr_nonexistent_XXXXX.bin", hash, 10);
    EXPECT_FALSE(result.has_value());
}

TEST(PartialFile, SaveAndLoadRoundTrip) {
    auto out = make_tempfile();
    // pre-allocate output file to expected size (chunk_count * CHUNK_SIZE)
    ::ftruncate(out.fd, static_cast<off_t>(4 * CHUNK_SIZE));

    auto hash = make_fake_hash(0x42);
    ChunkManager cm(4);
    cm.mark_done(0);
    cm.mark_done(2);

    PartialFile::save(out.path, hash, cm);

    auto loaded = PartialFile::load(out.path, hash, 4);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_FALSE(loaded->needs_chunk(0));  // done
    EXPECT_TRUE(loaded->needs_chunk(1));   // needed
    EXPECT_FALSE(loaded->needs_chunk(2));  // done
    EXPECT_TRUE(loaded->needs_chunk(3));   // needed

    PartialFile::remove(out.path);
}

TEST(PartialFile, LoadReturnsNulloptOnHashMismatch) {
    auto out = make_tempfile();
    ::ftruncate(out.fd, static_cast<off_t>(4 * CHUNK_SIZE));

    auto hash_a = make_fake_hash(0x11);
    auto hash_b = make_fake_hash(0x22);

    ChunkManager cm(4);
    cm.mark_done(0);
    PartialFile::save(out.path, hash_a, cm);

    auto loaded = PartialFile::load(out.path, hash_b, 4);
    EXPECT_FALSE(loaded.has_value());

    PartialFile::remove(out.path);
}

TEST(PartialFile, LoadReturnsNulloptIfOutputFileMissing) {
    // save partial file pointing to an output that doesn't exist
    auto out = make_tempfile();
    ::ftruncate(out.fd, static_cast<off_t>(4 * CHUNK_SIZE));

    auto hash = make_fake_hash(0x55);
    ChunkManager cm(4);
    cm.mark_done(0);
    PartialFile::save(out.path, hash, cm);

    // delete the output file but leave the partial
    std::string partial_path = PartialFile::path_for(out.path);
    ::unlink(out.path.c_str());
    out.fd = -1;

    auto loaded = PartialFile::load(out.path, hash, 4);
    EXPECT_FALSE(loaded.has_value());

    ::unlink(partial_path.c_str());
}

TEST(PartialFile, RemoveIsSilentIfNoFile) {
    // should not throw
    EXPECT_NO_THROW(PartialFile::remove("/tmp/embr_nonexistent_XXXXX.bin"));
}
