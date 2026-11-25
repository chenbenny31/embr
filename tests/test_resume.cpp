//
// Created by benny on 4/20/26.
//

// tests/test_resume.cpp

#include "core/chunk_manager.hpp"
#include "core/partial_file.hpp"
#include "core/hash.hpp"
#include "core/pull.hpp"
#include "core/push.hpp"
#include "transport/tcp_client.hpp"
#include "transport/tcp_server.hpp"
#include "util/constants.hpp"
#include "util/socket_fd.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>
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
    auto result = PartialFile::load("/tmp/embr_nonexistent_XXXXX.bin", hash, 10, 10 * CHUNK_SIZE);
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

    auto loaded = PartialFile::load(out.path, hash, 4, 4 * CHUNK_SIZE);
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

    auto loaded = PartialFile::load(out.path, hash_b, 4, 4 * CHUNK_SIZE);
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

    auto loaded = PartialFile::load(out.path, hash, 4, 4 * CHUNK_SIZE);
    EXPECT_FALSE(loaded.has_value());

    ::unlink(partial_path.c_str());
}

TEST(PartialFile, LoadAcceptsUnalignedFileSize) {
    auto out = make_tempfile();
    const uint64_t file_size = 3 * CHUNK_SIZE + 12345; // 4 chunks, last one short
    ::ftruncate(out.fd, static_cast<off_t>(file_size)); // what run_pull does on a fresh pull

    auto hash = make_fake_hash(0x33);
    ChunkManager cm(4);
    cm.mark_done(1);
    PartialFile::save(out.path, hash, cm);

    auto loaded = PartialFile::load(out.path, hash, 4, file_size);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_FALSE(loaded->needs_chunk(1));

    PartialFile::remove(out.path);
}

TEST(PartialFile, LoadReturnsNulloptOnFileSizeMismatch) {
    auto out = make_tempfile();
    ::ftruncate(out.fd, static_cast<off_t>(4 * CHUNK_SIZE));

    auto hash = make_fake_hash(0x44);
    ChunkManager cm(4);
    cm.mark_done(0);
    PartialFile::save(out.path, hash, cm);

    EXPECT_FALSE(PartialFile::load(out.path, hash, 4, 4 * CHUNK_SIZE - 1).has_value());

    PartialFile::remove(out.path);
}

TEST(PartialFile, SaveLeavesNoTempFile) {
    auto out = make_tempfile();
    ::ftruncate(out.fd, static_cast<off_t>(4 * CHUNK_SIZE));
    ChunkManager cm(4);
    PartialFile::save(out.path, make_fake_hash(0x66), cm);

    struct stat st{};
    EXPECT_EQ(::stat(PartialFile::path_for(out.path).c_str(), &st), 0);
    EXPECT_NE(::stat((PartialFile::path_for(out.path) + ".tmp").c_str(), &st), 0);

    PartialFile::remove(out.path);
}

TEST(PartialFile, RemoveIsSilentIfNoFile) {
    // should not throw
    EXPECT_NO_THROW(PartialFile::remove("/tmp/embr_nonexistent_XXXXX.bin"));
}

// ── run_pull against a real run_push over loopback TCP ───────────────────────

static std::vector<uint8_t> read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

static void write_all(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// source file of n bytes with a non-repeating pattern; removes its hash cache on destruction
struct SourceFile {
    TempFile file;
    std::vector<uint8_t> bytes;

    explicit SourceFile(size_t n) : file(make_tempfile()), bytes(n) {
        uint32_t x = 2463534242u;
        for (auto& b : bytes) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = static_cast<uint8_t>(x); }
        write_all(file.path, bytes);
    }
    ~SourceFile() { ::unlink(hash_cache_path_for(file.path).c_str()); }
};

// one transfer; returns the pull side's error text, empty on success.
// edit_meta changes what the sender announces, to stand in for data that does not match its hash
static std::string pull_from_push(const std::string& src_path, const std::string& out_path,
                                  const std::function<void(FileMeta&)>& edit_meta = {}) {
    ::signal(SIGPIPE, SIG_IGN); // main.cpp does this for the real binary

    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t addr_len = sizeof(addr);
    if (lfd < 0 || ::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(lfd, 1) < 0 ||
        ::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
        throw std::runtime_error("pull_from_push: listen failed");
    }
    SocketFd listen_fd{lfd};

    std::jthread pusher([&src_path, &listen_fd, &edit_meta]() {
        try {
            FileMeta meta = precompute_meta(src_path);
            if (edit_meta) { edit_meta(meta); }
            SocketFd file_fd{::open(src_path.c_str(), O_RDONLY)};
            auto conn = tcp_accept(listen_fd.get());
            run_push(*conn, std::move(file_fd), std::move(meta));
        } catch (const std::exception&) {} // a pull that stops early ends the push with an error
    });

    try {
        auto conn = tcp_connect("127.0.0.1", ntohs(addr.sin_port));
        run_pull(*conn, out_path);
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

TEST(RunPull, FreshTransferOfUnalignedFile) {
    SourceFile src(2 * CHUNK_SIZE + 12345);
    auto out = make_tempfile();

    EXPECT_EQ(pull_from_push(src.file.path, out.path), "");
    EXPECT_EQ(read_all(out.path), src.bytes);
}

// EMBR_VERIFY for one scope: "inline" hashes on the receiving thread, "thread" on at least one other
struct VerifyMode {
    explicit VerifyMode(const char* mode) { ::setenv("EMBR_VERIFY", mode, 1); }
    ~VerifyMode() { ::unsetenv("EMBR_VERIFY"); }
};

// EMBR_VERIFY_THREADS for one scope
struct VerifyThreads {
    explicit VerifyThreads(const char* count) { ::setenv("EMBR_VERIFY_THREADS", count, 1); }
    ~VerifyThreads() { ::unsetenv("EMBR_VERIFY_THREADS"); }
};

TEST(RunPull, VerifierThreadAndInlineGiveTheSameFile) {
    SourceFile src(5 * CHUNK_SIZE + 12345);
    for (const char* mode : {"thread", "inline"}) {
        VerifyMode verify(mode);
        TempFile out = make_tempfile();
        EXPECT_EQ(pull_from_push(src.file.path, out.path), "") << mode;
        EXPECT_EQ(read_all(out.path), src.bytes) << mode;
        EXPECT_NE(::access(PartialFile::path_for(out.path).c_str(), F_OK), 0) << mode;
    }
}

TEST(RunPull, SeveralVerifierThreadsGiveTheSameFile) {
    SourceFile src(11 * CHUNK_SIZE + 12345);
    VerifyThreads verify("3");
    TempFile out = make_tempfile();
    // chunks 2 and 7 are announced with wrong hashes: results arrive in any order and both must be caught
    EXPECT_EQ(pull_from_push(src.file.path, out.path, [](FileMeta& meta) {
        meta.chunk_hashes[2][0] ^= 0xff;
        meta.chunk_hashes[7][0] ^= 0xff;
    }), "");
    EXPECT_EQ(read_all(out.path), src.bytes); // the data is right; only the announced hashes were not

    FileMeta announced = precompute_meta(src.file.path);
    announced.chunk_hashes[2][0] ^= 0xff;
    announced.chunk_hashes[7][0] ^= 0xff;
    auto record = PartialFile::load(out.path, announced.chunk_hashes[0],
                                    announced.chunk_count, announced.file_size);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->needed_chunks(), (std::vector<uint32_t>{2, 7}));
    PartialFile::remove(out.path);
}

TEST(RunPull, ManyChunksAreVerifiedInSetsOfLanes) {
    // more chunks than any set of lanes, so the verifier hashes full sets while the transfer runs
    SourceFile src(40 * CHUNK_SIZE + 12345);
    TempFile out = make_tempfile();
    EXPECT_EQ(pull_from_push(src.file.path, out.path, [](FileMeta& meta) {
        meta.chunk_hashes[20][0] ^= 0xff;
        meta.chunk_hashes[40][0] ^= 0xff; // the short last chunk
    }), "");
    EXPECT_EQ(read_all(out.path), src.bytes);

    FileMeta announced = precompute_meta(src.file.path);
    announced.chunk_hashes[20][0] ^= 0xff;
    announced.chunk_hashes[40][0] ^= 0xff;
    auto record = PartialFile::load(out.path, announced.chunk_hashes[0],
                                    announced.chunk_count, announced.file_size);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->needed_chunks(), (std::vector<uint32_t>{20, 40}));
    PartialFile::remove(out.path);
}

TEST(RunPull, ChunkThatFailsItsHashIsLeftForResume) {
    SourceFile src(5 * CHUNK_SIZE + 12345);
    for (const char* mode : {"thread", "inline"}) {
        VerifyMode verify(mode);
        TempFile out = make_tempfile();
        // the sender announces a wrong hash for chunk 3, so the data it sends cannot match
        EXPECT_EQ(pull_from_push(src.file.path, out.path,
                                 [](FileMeta& meta) { meta.chunk_hashes[3][0] ^= 0xff; }), "") << mode;

        FileMeta announced = precompute_meta(src.file.path);
        announced.chunk_hashes[3][0] ^= 0xff;
        auto record = PartialFile::load(out.path, announced.chunk_hashes[0],
                                        announced.chunk_count, announced.file_size);
        ASSERT_TRUE(record.has_value()) << mode;
        EXPECT_EQ(record->needed_chunks(), std::vector<uint32_t>{3}) << mode;
        PartialFile::remove(out.path);
    }
}

TEST(RunPull, ResumeKeepsVerifiedChunksAndRefetchesStaleOnes) {
    SourceFile src(2 * CHUNK_SIZE + 12345); // 3 chunks
    auto out = make_tempfile();

    // an interrupted pull: chunk 0 really on disk, chunk 1 marked done but never written
    // (the power-loss case), chunk 2 not received
    std::vector<uint8_t> on_disk(src.bytes.size(), 0);
    std::copy(src.bytes.begin(), src.bytes.begin() + CHUNK_SIZE, on_disk.begin());
    write_all(out.path, on_disk);
    ChunkManager cm(3);
    cm.mark_done(0);
    cm.mark_done(1);
    PartialFile::save(out.path, sha256_buf(src.bytes.data(), CHUNK_SIZE), cm);

    testing::internal::CaptureStdout();
    const std::string err = pull_from_push(src.file.path, out.path);
    const std::string log = testing::internal::GetCapturedStdout();

    EXPECT_EQ(err, "");
    EXPECT_NE(log.find("resuming partial download - 1 chunks verified on disk"), std::string::npos) << log;
    EXPECT_NE(log.find("requesting 2/3 chunks"), std::string::npos) << log;
    EXPECT_EQ(read_all(out.path), src.bytes);
    struct stat st{};
    EXPECT_NE(::stat(PartialFile::path_for(out.path).c_str(), &st), 0); // removed on completion
}

TEST(RunPull, SecondPullOfTheSameFileIsAlreadyComplete) {
    SourceFile src(2 * CHUNK_SIZE + 12345);
    auto out = make_tempfile();
    ASSERT_EQ(pull_from_push(src.file.path, out.path), "");

    struct stat before{};
    ASSERT_EQ(::stat(out.path.c_str(), &before), 0);

    testing::internal::CaptureStdout();
    const std::string err = pull_from_push(src.file.path, out.path);
    const std::string log = testing::internal::GetCapturedStdout();

    struct stat after{};
    ASSERT_EQ(::stat(out.path.c_str(), &after), 0);
    EXPECT_EQ(err, "");
    EXPECT_NE(log.find("already complete"), std::string::npos) << log;
    EXPECT_EQ(log.find("requesting"), std::string::npos) << log; // no chunk was asked for
    EXPECT_EQ(before.st_mtim.tv_nsec, after.st_mtim.tv_nsec);     // and the file was not written
    EXPECT_EQ(before.st_mtim.tv_sec, after.st_mtim.tv_sec);
    EXPECT_EQ(read_all(out.path), src.bytes);
}

TEST(RunPull, SenderChosenNameNeverReplacesAnExistingFile) {
    SourceFile src(CHUNK_SIZE / 2);
    const std::string name = std::filesystem::path(src.file.path).filename().string();

    // pull with no --out writes the sender's file name into the current directory
    char dir_tmpl[] = "/tmp/embr_pull_dir_XXXXXX";
    ASSERT_NE(::mkdtemp(dir_tmpl), nullptr);
    const std::filesystem::path old_cwd = std::filesystem::current_path();
    std::filesystem::current_path(dir_tmpl);

    const std::vector<uint8_t> precious = {'k', 'e', 'e', 'p'};
    write_all(name, precious);
    const std::string err = pull_from_push(src.file.path, "");
    const std::vector<uint8_t> after = read_all(name);

    std::filesystem::current_path(old_cwd);
    std::filesystem::remove_all(dir_tmpl);

    EXPECT_NE(err.find("already exists and is a different file"), std::string::npos) << err;
    EXPECT_EQ(after, precious);
}

TEST(RunPull, ExplicitOutPathOverwrites) {
    SourceFile src(CHUNK_SIZE / 2);
    auto out = make_tempfile();
    write_all(out.path, {'o', 'l', 'd'});

    EXPECT_EQ(pull_from_push(src.file.path, out.path), "");
    EXPECT_EQ(read_all(out.path), src.bytes);
}

// more recorded chunks than a set of lanes: the stale ones are found wherever they sit, the short
// last chunk included
TEST(RunPull, ResumeChecksManyRecordedChunksAndRefetchesTheStaleOnes) {
    SourceFile src(40 * CHUNK_SIZE + 12345); // 41 chunks
    auto out = make_tempfile();

    // chunk 20 was never received; 3, 17 and the last one are recorded as received but hold zeros
    std::vector<uint8_t> on_disk = src.bytes;
    for (const size_t chunk : {3u, 17u, 20u, 40u}) {
        const size_t begin = chunk * CHUNK_SIZE;
        std::fill(on_disk.begin() + begin, on_disk.begin() + std::min(begin + CHUNK_SIZE, on_disk.size()), 0);
    }
    write_all(out.path, on_disk);
    ChunkManager cm(41);
    for (uint32_t i = 0; i < 41; ++i) {
        if (i != 20) { cm.mark_done(i); }
    }
    PartialFile::save(out.path, sha256_buf(src.bytes.data(), CHUNK_SIZE), cm);

    testing::internal::CaptureStdout();
    const std::string err = pull_from_push(src.file.path, out.path);
    const std::string log = testing::internal::GetCapturedStdout();

    EXPECT_EQ(err, "");
    EXPECT_NE(log.find("resuming partial download - 37 chunks verified on disk"), std::string::npos) << log;
    EXPECT_NE(log.find("requesting 4/41 chunks"), std::string::npos) << log;
    EXPECT_EQ(read_all(out.path), src.bytes);
}

// the right size is not enough: one wrong byte far into the file and it is fetched again
TEST(RunPull, SameSizeFileWithOneWrongByteIsNotAlreadyComplete) {
    SourceFile src(40 * CHUNK_SIZE + 12345);
    auto out = make_tempfile();
    std::vector<uint8_t> other = src.bytes;
    other[37 * CHUNK_SIZE + 5] ^= 0xff;
    write_all(out.path, other);

    testing::internal::CaptureStdout();
    const std::string err = pull_from_push(src.file.path, out.path);
    const std::string log = testing::internal::GetCapturedStdout();

    EXPECT_EQ(err, "");
    EXPECT_EQ(log.find("already complete"), std::string::npos) << log;
    EXPECT_NE(log.find("requesting 41/41 chunks"), std::string::npos) << log;
    EXPECT_EQ(read_all(out.path), src.bytes);
}
