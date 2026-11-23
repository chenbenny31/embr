// SHA-256 of 1 MiB chunks, one thread: OpenSSL per chunk (what embr does) against ISA-L crypto's
// multi-buffer manager, which hashes several independent chunks at once in SIMD lanes.
//   mbbench <file> [repeats]
// Every multi-buffer digest is compared with OpenSSL's.
#include <isa-l_crypto/sha256_mb.h>
#include <openssl/evp.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
// the library's per-CPU entry points; the public isal_* calls pick one of these at run time
#define DECLARE(arch)                                                                              \
    void _sha256_ctx_mgr_init_##arch(ISAL_SHA256_HASH_CTX_MGR* mgr);                               \
    ISAL_SHA256_HASH_CTX* _sha256_ctx_mgr_submit_##arch(ISAL_SHA256_HASH_CTX_MGR* mgr,             \
                                                         ISAL_SHA256_HASH_CTX* ctx,                \
                                                         const void* buffer, uint32_t len,         \
                                                         ISAL_HASH_CTX_FLAG flags);                \
    ISAL_SHA256_HASH_CTX* _sha256_ctx_mgr_flush_##arch(ISAL_SHA256_HASH_CTX_MGR* mgr);
DECLARE(avx2)
DECLARE(avx512)
DECLARE(avx512_ni)
DECLARE(sse_ni)
}

namespace {

constexpr size_t CHUNK = 1024 * 1024;
using Digest = std::array<uint8_t, 32>;

Digest openssl_sha256(const uint8_t* data, size_t len) {
    Digest out{};
    unsigned int n = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, out.data(), &n);
    EVP_MD_CTX_free(ctx);
    return out;
}

Digest from_words(const ISAL_SHA256_HASH_CTX& ctx) {
    Digest out{};
    for (int i = 0; i < 8; ++i) {
        const uint32_t word = ctx.job.result_digest[i];
        out[i * 4] = static_cast<uint8_t>(word >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(word >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(word >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(word);
    }
    return out;
}

struct Arch {
    const char* name;
    void (*init)(ISAL_SHA256_HASH_CTX_MGR*);
    ISAL_SHA256_HASH_CTX* (*submit)(ISAL_SHA256_HASH_CTX_MGR*, ISAL_SHA256_HASH_CTX*, const void*, uint32_t, ISAL_HASH_CTX_FLAG);
    ISAL_SHA256_HASH_CTX* (*flush)(ISAL_SHA256_HASH_CTX_MGR*);
};

// the library's own choice for this CPU, through the public calls
void init_auto(ISAL_SHA256_HASH_CTX_MGR* mgr) { isal_sha256_ctx_mgr_init(mgr); }
ISAL_SHA256_HASH_CTX* submit_auto(ISAL_SHA256_HASH_CTX_MGR* mgr, ISAL_SHA256_HASH_CTX* ctx, const void* buffer,
                                  uint32_t len, ISAL_HASH_CTX_FLAG flags) {
    ISAL_SHA256_HASH_CTX* done = nullptr;
    isal_sha256_ctx_mgr_submit(mgr, ctx, &done, buffer, len, flags);
    return done;
}
ISAL_SHA256_HASH_CTX* flush_auto(ISAL_SHA256_HASH_CTX_MGR* mgr) {
    ISAL_SHA256_HASH_CTX* done = nullptr;
    isal_sha256_ctx_mgr_flush(mgr, &done);
    return done;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: mbbench <file> [repeats]\n"); return 2; }
    const int repeats = argc > 2 ? std::atoi(argv[2]) : 3;
    const int fd = ::open(argv[1], O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) != 0) { std::perror("open"); return 1; }
    const size_t size = static_cast<size_t>(st.st_size);
    const auto* data = static_cast<const uint8_t*>(::mmap(nullptr, size, PROT_READ, MAP_SHARED | MAP_POPULATE, fd, 0));
    if (data == MAP_FAILED) { std::perror("mmap"); return 1; }
    const size_t chunks = size / CHUNK;
    const double megabytes = static_cast<double>(chunks * CHUNK) / 1e6;

    std::vector<Digest> expected(chunks);
    double best = 1e9;
    for (int r = 0; r < repeats; ++r) {
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < chunks; ++i) { expected[i] = openssl_sha256(data + i * CHUNK, CHUNK); }
        best = std::min(best, seconds_since(start));
    }
    std::printf("%-34s %8.0f MB/s  %.3f s per GiB\n", "openssl, one chunk at a time", megabytes / best, best * 1073.741824 / megabytes);

    const Arch archs[] = {
        {"multi-buffer, library's choice", init_auto, submit_auto, flush_auto},
        {"multi-buffer, avx2 (8 lanes)", _sha256_ctx_mgr_init_avx2, _sha256_ctx_mgr_submit_avx2, _sha256_ctx_mgr_flush_avx2},
        {"multi-buffer, avx512 (16 lanes)", _sha256_ctx_mgr_init_avx512, _sha256_ctx_mgr_submit_avx512, _sha256_ctx_mgr_flush_avx512},
        {"multi-buffer, avx512 + sha ext", _sha256_ctx_mgr_init_avx512_ni, _sha256_ctx_mgr_submit_avx512_ni, _sha256_ctx_mgr_flush_avx512_ni},
        {"multi-buffer, sse + sha ext", _sha256_ctx_mgr_init_sse_ni, _sha256_ctx_mgr_submit_sse_ni, _sha256_ctx_mgr_flush_sse_ni},
    };
    // the manager and the contexts want 64- and 16-byte alignment
    std::unique_ptr<ISAL_SHA256_HASH_CTX_MGR, void (*)(void*)> mgr(
        static_cast<ISAL_SHA256_HASH_CTX_MGR*>(std::aligned_alloc(64, (sizeof(ISAL_SHA256_HASH_CTX_MGR) + 63) / 64 * 64)), std::free);
    std::unique_ptr<ISAL_SHA256_HASH_CTX[], void (*)(void*)> ctxs(
        static_cast<ISAL_SHA256_HASH_CTX*>(std::aligned_alloc(64, (sizeof(ISAL_SHA256_HASH_CTX) * chunks + 63) / 64 * 64)), std::free);

    for (const Arch& arch : archs) {
        // batch = chunks handed over before a flush: what a verifier with that many chunks waiting would do
        for (size_t batch : {size_t{1}, size_t{4}, size_t{8}, size_t{16}, size_t{32}}) {
            best = 1e9;
            for (int r = 0; r < repeats; ++r) {
                for (size_t i = 0; i < chunks; ++i) { isal_hash_ctx_init(&ctxs[i]); }
                arch.init(mgr.get());
                const auto start = std::chrono::steady_clock::now();
                for (size_t i = 0; i < chunks; ++i) {
                    arch.submit(mgr.get(), &ctxs[i], data + i * CHUNK, static_cast<uint32_t>(CHUNK), ISAL_HASH_ENTIRE);
                    if ((i + 1) % batch == 0 || i + 1 == chunks) {
                        while (arch.flush(mgr.get()) != nullptr) {}
                    }
                }
                best = std::min(best, seconds_since(start));
            }
            size_t wrong = 0;
            for (size_t i = 0; i < chunks; ++i) { wrong += from_words(ctxs[i]) != expected[i]; }
            std::printf("%-34s batch %2zu  %8.0f MB/s  %.3f s per GiB  %s\n", arch.name, batch, megabytes / best,
                        best * 1073.741824 / megabytes, wrong == 0 ? "digests match" : "DIGESTS DIFFER");
        }
    }
    return 0;
}
