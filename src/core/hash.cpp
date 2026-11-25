//
// Created by benny on 3/23/26.
//

#include "hash.hpp"
#include "util/constants.hpp"
#include "util/exact_io.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <openssl/evp.h>
#ifdef EMBR_HAVE_ISAL_CRYPTO
#include <isa-l_crypto/sha256_mb.h>
#include <cpuid.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <exception>
#include <mutex>
#include <numeric>
#include <thread>
#include <cstdint>

// .embr.hash format:
//   [file_size:u64][mtime_ns:u64][chunk_size:u32][chunk_count:u32]
//   [chunk_hashes: chunk_count * HASH_SIZE bytes]

namespace {

// RAII wrapper for EVP_MD_CTX, guarantees free on throw
struct EvpCtx {
    EVP_MD_CTX* ctx;
    EvpCtx() : ctx(EVP_MD_CTX_new()) {
        if (!ctx) { throw std::runtime_error("EVP_MD_CTX_new failed"); }
    }
    ~EvpCtx() { EVP_MD_CTX_free(ctx); }
    EvpCtx(const EvpCtx&) = delete;
    EvpCtx& operator=(const EvpCtx&) = delete;
};

#ifdef EMBR_HAVE_ISAL_CRYPTO
// lanes of the code ISA-L picks on this CPU: 16 with AVX-512, 8 with AVX2. With SHA extensions
// and no AVX-512 it hashes one buffer at a time with them, as OpenSSL does: nothing to gain.
unsigned isal_lanes() {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl") &&
        __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512dq") &&
        __builtin_cpu_supports("avx512cd")) {
        return 16;
    }
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    const bool sha_extensions = __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) != 0 && (ebx & (1u << 29)) != 0;
    return __builtin_cpu_supports("avx2") && !sha_extensions ? 8 : 1;
}

void isal_check(int rc, const char* call) {
    if (rc != 0) {
        throw std::runtime_error(std::string("sha256_many: ") + call + " failed: " + std::to_string(rc));
    }
}

// every input as one whole job; the manager runs them as lanes fill and on the final flushes
void sha256_side_by_side(std::span<const HashInput> inputs,
                         std::span<std::array<uint8_t, HASH_SIZE>> digests) {
    auto manager = std::make_unique<ISAL_SHA256_HASH_CTX_MGR>(); // over-aligned type: aligned new
    auto contexts = std::make_unique<ISAL_SHA256_HASH_CTX[]>(inputs.size());
    isal_check(isal_sha256_ctx_mgr_init(manager.get()), "init");
    ISAL_SHA256_HASH_CTX* done = nullptr;
    static const uint8_t no_bytes = 0; // the library refuses a null source even for an empty buffer
    for (size_t i = 0; i < inputs.size(); ++i) {
        isal_hash_ctx_init(&contexts[i]);
        const uint8_t* source = inputs[i].len == 0 ? &no_bytes : inputs[i].data;
        isal_check(isal_sha256_ctx_mgr_submit(manager.get(), &contexts[i], &done, source,
                                              static_cast<uint32_t>(inputs[i].len), ISAL_HASH_ENTIRE),
                   "submit");
    }
    do {
        isal_check(isal_sha256_ctx_mgr_flush(manager.get(), &done), "flush");
    } while (done != nullptr);

    for (size_t i = 0; i < inputs.size(); ++i) {
        if (!isal_hash_ctx_complete(&contexts[i])) {
            throw std::runtime_error("sha256_many: a job did not complete");
        }
        for (size_t word = 0; word < ISAL_SHA256_DIGEST_NWORDS; ++word) { // host-order words, big-endian digest
            const uint32_t value = contexts[i].job.result_digest[word];
            digests[i][word * 4] = static_cast<uint8_t>(value >> 24);
            digests[i][word * 4 + 1] = static_cast<uint8_t>(value >> 16);
            digests[i][word * 4 + 2] = static_cast<uint8_t>(value >> 8);
            digests[i][word * 4 + 3] = static_cast<uint8_t>(value);
        }
    }
}
#endif

}

std::array<uint8_t, HASH_SIZE> sha256_buf(const uint8_t* data, size_t len) {
    EvpCtx evp;
    if (!EVP_DigestInit_ex(evp.ctx, EVP_sha256(), nullptr)) {
        throw std::runtime_error("sha256_buf: EVP_DigestInit_ex() failed");
    }
    if (!EVP_DigestUpdate(evp.ctx, data, len)) {
        throw std::runtime_error("sha256_buf: EVP_DigestUpdate() failed");
    }
    std::array<uint8_t, HASH_SIZE> digest{};
    unsigned int digest_len = 0;
    if (!EVP_DigestFinal_ex(evp.ctx, digest.data(), &digest_len)) {
        throw std::runtime_error("sha256_buf: EVP_DigestFinal_ex() failed");
    }
    return digest;
}

unsigned sha256_lanes() {
    static const unsigned lanes = [] {
        unsigned detected = 1;
#ifdef EMBR_HAVE_ISAL_CRYPTO
        detected = isal_lanes();
        if (const char* forced = std::getenv("EMBR_HASH_LANES")) {
            detected = static_cast<unsigned>(std::clamp(std::atoi(forced), 1, 64));
        }
        if (detected > 1) {
            // the library chooses its code for this CPU on the first call of each entry point and
            // stores the choice unsynchronised: make those calls here, once, before threads hash
            const uint8_t byte = 0;
            const HashInput input{&byte, 1};
            std::array<uint8_t, HASH_SIZE> digest{};
            sha256_side_by_side(std::span(&input, 1), std::span(&digest, 1));
        }
#endif
        return detected;
    }();
    return lanes;
}

void sha256_many(std::span<const HashInput> inputs,
                 std::span<std::array<uint8_t, HASH_SIZE>> digests) {
    if (inputs.size() != digests.size()) {
        throw std::runtime_error("sha256_many: inputs and digests differ in count");
    }
#ifdef EMBR_HAVE_ISAL_CRYPTO
    // half-empty lanes are slower than one buffer at a time; job lengths are 32-bit
    const bool fits = std::all_of(inputs.begin(), inputs.end(), [](const HashInput& input) {
        return input.len <= std::numeric_limits<uint32_t>::max();
    });
    if (sha256_lanes() > 1 && inputs.size() * 2 >= sha256_lanes() && fits) {
        sha256_side_by_side(inputs, digests);
        return;
    }
#endif
    for (size_t i = 0; i < inputs.size(); ++i) {
        digests[i] = sha256_buf(inputs[i].data, inputs[i].len);
    }
}

std::string hash_cache_path_for(const std::string& file_path) {
    if (file_path.empty()) {
        throw std::runtime_error("hash_cache_path_for: file_path is empty");
    }
    const size_t slash_pos = file_path.rfind('/');
    if (slash_pos == std::string::npos) {
        return '.' + file_path + ".embr.hash";
    }
    return file_path.substr(0, slash_pos + 1) + '.' +
           file_path.substr(slash_pos + 1) + ".embr.hash";
}

bool hash_cache_load(const std::string& file_path,
                     uint64_t file_size,
                     uint64_t mtime_ns,
                     uint32_t chunk_count,
                     std::vector<std::array<uint8_t, HASH_SIZE>>& chunk_hashes) {
    const std::string cache_path = hash_cache_path_for(file_path);
    int fd = ::open(cache_path.c_str(), O_RDONLY);
    if (fd < 0) { return false; }

    try {
        uint64_t stored_size{};
        uint64_t stored_mtime{};
        uint32_t stored_chunk_size{};
        uint32_t stored_count{};

        read_exact(fd, reinterpret_cast<uint8_t*>(&stored_size), sizeof(stored_size));
        read_exact(fd, reinterpret_cast<uint8_t*>(&stored_mtime), sizeof(stored_mtime));
        read_exact(fd, reinterpret_cast<uint8_t*>(&stored_chunk_size), sizeof(stored_chunk_size));
        read_exact(fd, reinterpret_cast<uint8_t*>(&stored_count), sizeof(stored_count));

        if (stored_size != file_size ||
            stored_mtime != mtime_ns ||
            stored_count != chunk_count) {
            ::close(fd);
            return false;
        }

        chunk_hashes.resize(chunk_count);
        for (auto& hash : chunk_hashes) {
            read_exact(fd, hash.data(), HASH_SIZE);
        }
        ::close(fd);
        return true;

    } catch (const std::runtime_error& e) {
        ::close(fd);
        return false; // corrupted cache - fall back to re-compute
    }
}

void hash_cache_save(const std::string& file_path,
                     uint64_t file_size,
                     uint64_t mtime_ns,
                     uint32_t chunk_size,
                     uint32_t chunk_count,
                     const std::vector<std::array<uint8_t, HASH_SIZE>>& chunk_hashes) {
    const std::string cache_path = hash_cache_path_for(file_path);
    int fd = ::open(cache_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error("hash_cache_save: failed to open: " + cache_path);
    }

    try {
        write_exact(fd, reinterpret_cast<uint8_t*>(&file_size), sizeof(file_size));
        write_exact(fd, reinterpret_cast<uint8_t*>(&mtime_ns), sizeof(mtime_ns));
        write_exact(fd, reinterpret_cast<uint8_t*>(&chunk_size), sizeof(chunk_size));
        write_exact(fd, reinterpret_cast<uint8_t*>(&chunk_count), sizeof(chunk_count));

        for (const auto& hash : chunk_hashes) {
            write_exact(fd, hash.data(), HASH_SIZE);
        }
        ::close(fd);

    } catch (...) {
        ::close(fd);
        throw;
    }
}


std::vector<std::array<uint8_t, HASH_SIZE>> hash_chunks_parallel(int file_fd,
                                                                 uint64_t file_size,
                                                                 std::span<const uint32_t> chunk_indexes) {
    const auto chunk_count = static_cast<uint32_t>(chunk_indexes.size());
    std::vector<std::array<uint8_t, HASH_SIZE>> chunk_hashes(chunk_count);
    if (chunk_count == 0) { return chunk_hashes; }
    for (const uint32_t chunk_index : chunk_indexes) {
        if (static_cast<uint64_t>(chunk_index) * CHUNK_SIZE >= file_size) {
            throw std::out_of_range("hash_chunks_parallel: chunk " + std::to_string(chunk_index) +
                                    " starts past the end of the file");
        }
    }

    void* mapped = ::mmap(nullptr, static_cast<size_t>(file_size),
                          PROT_READ, MAP_SHARED, file_fd, 0);
    if (mapped == MAP_FAILED) {
        throw std::runtime_error(std::string("hash_chunks_parallel: mmap failed:") +
                                 std::strerror(errno));
    }

    // count on its own cache line, prevents false sharing between fetch_add and any adj hot data
    struct alignas(64) AlignedCounter {
        std::atomic<uint32_t> value{0};
    } next_chunk;

    // exception propagation, main thread re-throws workers' stored first exception
    std::vector<std::exception_ptr> worker_exceptions;
    std::mutex exceptions_mtx;

    const uint32_t lanes = sha256_lanes(); // at most 64
    const uint32_t num_workers = std::min((chunk_count + lanes - 1) / lanes, // a worker takes a set of lanes
                                          static_cast<uint32_t>(std::max(1u,
                                              std::thread::hardware_concurrency())));

    worker_exceptions.resize(num_workers, nullptr);

    auto worker = [&](uint32_t worker_id) {
        try {
            while (true) {
                // memory order relaxed: each independently grabs the next set of listed chunks,
                // one set of lanes at a time so sha256_many can hash them side by side
                const uint32_t first = next_chunk.value.fetch_add(lanes,
                                                                  std::memory_order_relaxed);
                if (first >= chunk_count) { break; }
                const uint32_t count = std::min(lanes, chunk_count - first);

                std::array<HashInput, 64> inputs{};
                for (uint32_t k = 0; k < count; ++k) {
                    const uint64_t offset = static_cast<uint64_t>(chunk_indexes[first + k]) * CHUNK_SIZE;
                    inputs[k] = {static_cast<const uint8_t*>(mapped) + offset,
                                 static_cast<size_t>(std::min(static_cast<uint64_t>(CHUNK_SIZE),
                                                              file_size - offset))};
                }
                sha256_many(std::span(inputs.data(), count),
                            std::span(chunk_hashes.data() + first, count));
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(exceptions_mtx);
            worker_exceptions[worker_id] = std::current_exception();
        }
    };

    {
        std::vector<std::jthread> workers;
        workers.reserve(num_workers);
        for (uint32_t t = 0; t < num_workers; t++) {
            workers.emplace_back(worker, t);
        }
    }

    ::munmap(mapped, static_cast<size_t>(file_size));

    for (const auto& ex : worker_exceptions) {
        if (ex) { std::rethrow_exception(ex); }
    }

    return chunk_hashes;
}

std::vector<std::array<uint8_t, HASH_SIZE>> hash_compute_parallel(int file_fd,
                                                                  uint64_t file_size,
                                                                  uint32_t chunk_count) {
    std::vector<uint32_t> every_chunk(chunk_count);
    std::iota(every_chunk.begin(), every_chunk.end(), 0u);
    return hash_chunks_parallel(file_fd, file_size, every_chunk);
}