//
// Created by benny on 9/6/26.
//


// track the type-erased Buffer release costs per lifetime

#include "core/protocol.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

static size_t g_allocs = 0; // proves the capture fits std::function's small-object buffer

void* operator new(size_t n) {
    g_allocs++;
    void* p = std::malloc(n);
    if (!p) { throw std::bad_alloc(); }
    return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

struct Pool {
    std::vector<unsigned> free_;
    void release(unsigned i) {
        free_.push_back(i);
    }
};

// the non-erased alter: a func ptr or ctx
struct RawBuf {
    uint8_t* ptr{};
    size_t size{};
    void (*rel)(void*, uint8_t*){};
    void* ctx{};
    ~RawBuf() { if (ptr && rel) { rel(ctx, ptr); } }
};

static void pool_rel(void* c, uint8_t*) { static_cast<Pool*>(c)->release(0); }

template <class F> double bench(const char* name, size_t iters, F&& f) {
    g_allocs = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t k = 0; k < iters; k++) { f(k); }
    const double ns = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - t0).count() / iters;
    std::printf("%-46s %7.2f ns/op heap allocs/op=%.3f\n", name, ns, double(g_allocs) / iters);
    return ns;
}

int main() {
    Pool pool;
    pool.free_.reserve(1 << 20);
    uint8_t slot[1350];
    const size_t N = 20'000'000;
    std::vector<Buffer> inflight(64);
    std::vector<RawBuf> rawfl(64);

    // egress pattern: construct with capturing lambda, move into the slot table, destroy on NOTIF
    const double e = bench("erased: Buffer(slot,n,[this,i]) + move + dtor", N, [&](size_t k) {
        const unsigned i = k & 63;
        inflight[i] = Buffer(slot, 1154, [&pool, i](uint8_t*) { pool.release(i); });
        inflight[i] = Buffer{};
        pool.free_.clear();
    });
    const double r = bench("fn-pointer+ctx struct, same pattern", N, [&](size_t k) {
        const unsigned i = k & 63;
        rawfl[i] = RawBuf{slot, 1154, pool_rel, &pool};
        rawfl[i] = RawBuf{};
        pool.free_.clear();
    });
    const double d = bench("direct: pool.release(i), no Buffer at all", N, [&](size_t k) {
        pool.release(k & 63);
        pool.free_.clear();
    });

    const double per_gib_pkts = 1073741824.0 / 1154.0; // QUIC stream payload per datagram
    const double per_gib_chunks = 1024.0; // 1 MiB mmap chunks
    std::printf("\nsizeof(Buffer)=%zu sizeof(RawBuf)=%zu\n", sizeof(Buffer), sizeof(RawBuf));
    std::printf("erasure vs fn-pointer: %.2f ns/Buffer -> %.2f ms/GiB per-datagram,"
                "%.1f us/GiB per-chunk\n", e - r, (e - r) * per_gib_pkts / 1e6,
                (e - r) * per_gib_chunks / 1e3);
    std::printf("erasure vs direct call: %.2f ns/Buffer -> %.2f ms/GiB per-datagram\n",
                 e - d, (e - d) * per_gib_pkts / 1e6);
}
