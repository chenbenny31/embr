// Created by benny on 9/6/26.
// Compare complete ownership representations in a construct/move/retire loop.

#include "core/protocol.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Counts intercepted ordinary scalar operator new calls in this single-threaded
// benchmark, not every possible allocation API or every std::function target.
static size_t g_allocs = 0;

void* operator new(size_t n) {
    ++g_allocs;
    void* p = std::malloc(n ? n : 1);
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

struct ReleaseCtx {
    Pool* pool{};
    unsigned index{};
};

// A function-pointer owner with inline state matching the capturing lambda.
// This is also type erasure; it is not a "no type erasure" control.
struct RawBuf {
    using Release = void (*)(void*, uint8_t*);

    uint8_t* ptr{};
    size_t size{};
    Release rel{};
    ReleaseCtx ctx{};

    RawBuf() = default;
    RawBuf(uint8_t* p, size_t n, Release r, ReleaseCtx c) noexcept
        : ptr(p), size(n), rel(r), ctx(c) {}

    RawBuf(const RawBuf&) = delete;
    RawBuf& operator=(const RawBuf&) = delete;

    RawBuf(RawBuf&& other) noexcept { take(other); }
    RawBuf& operator=(RawBuf&& other) noexcept {
        if (this != &other) {
            reset();
            take(other);
        }
        return *this;
    }

    ~RawBuf() { reset(); }

    void reset() noexcept {
        // The pool outlives the owners and has pre-reserved capacity, so the
        // release used here does not allocate or throw.
        if (ptr && rel) { rel(&ctx, ptr); }
        ptr = nullptr;
        size = 0;
        rel = nullptr;
        ctx = {};
    }

private:
    void take(RawBuf& other) noexcept {
        ptr = std::exchange(other.ptr, nullptr);
        size = std::exchange(other.size, 0);
        rel = std::exchange(other.rel, nullptr);
        ctx = std::exchange(other.ctx, ReleaseCtx{});
    }
};

static void pool_rel(void* context, uint8_t*) {
    const auto& c = *static_cast<const ReleaseCtx*>(context);
    c.pool->release(c.index);
}

static_assert(!std::is_copy_constructible_v<RawBuf>);
static_assert(!std::is_copy_assignable_v<RawBuf>);
static_assert(std::is_nothrow_move_constructible_v<RawBuf>);
static_assert(std::is_nothrow_move_assignable_v<RawBuf>);

// GNU compiler barrier: materialize the owner and preserve the release output.
// This is not a CPU fence, nor a guarantee of isolated callback timing.
inline void escape_memory(const void* p) noexcept {
    __asm__ __volatile__("" : : "g"(p) : "memory");
}

static void expect_releases(const Pool& pool, const char* owner,
                            const char* step,
                            std::initializer_list<unsigned> expected) {
    if (!std::equal(pool.free_.begin(), pool.free_.end(),
                    expected.begin(), expected.end())) {
        std::fprintf(stderr, "%s lifetime check failed: %s\n", owner, step);
        std::abort(); // Must remain active with NDEBUG.
    }
}

template <class Make>
void check_lifetime(const char* name, Pool& pool, Make make) {
    using Owner = decltype(make(0));
    pool.free_.clear();
    {
        auto dst = make(2);
        {
            auto src = make(7);
            auto moved = std::move(src);
            expect_releases(pool, name, "construction and move", {});
            dst = std::move(moved);
            expect_releases(pool, name, "replace occupied destination", {2});
        }
        expect_releases(pool, name, "destroy moved-from owners", {2});
        dst = Owner{};
        expect_releases(pool, name, "retire destination", {2, 7});
        dst = Owner{};
        expect_releases(pool, name, "retire empty destination", {2, 7});
    }
    expect_releases(pool, name, "destroy empty destination", {2, 7});
    {
        auto owner = make(9);
        expect_releases(pool, name, "retain until scope exit", {2, 7});
    }
    expect_releases(pool, name, "release at scope exit", {2, 7, 9});
    pool.free_.clear();
    std::printf("%s lifetime checks: PASS\n", name);
}

struct Sample {
    double ns;
    size_t new_calls;
};

template <class F> Sample bench(size_t iters, F&& f) {
    g_allocs = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t k = 0; k < iters; k++) { f(k); }
    const double ns = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - t0).count() / iters;
    return {ns, g_allocs};
}

template <size_t N>
void summarize(const char* name, std::array<double, N> values) {
    std::sort(values.begin(), values.end());
    const double median = (values[(N - 1) / 2] + values[N / 2]) / 2;
    std::printf("%-32s median %.3f ns/lifetime, range [%.3f, %.3f]\n",
                name, median, values.front(), values.back());
}

int main(int argc, char** argv) {
    const bool check_only = argc == 2 && std::string_view(argv[1]) == "--check-only";
    if (argc != 1 && !check_only) {
        std::fprintf(stderr, "Usage: %s [--check-only]\n", argv[0]);
        return 2;
    }
    Pool pool;
    pool.free_.reserve(64);
    uint8_t slot[1350]{};
    auto make_buffer = [&](unsigned i) {
        return Buffer(slot, 1154, [&pool, i](uint8_t*) { pool.release(i); });
    };
    auto make_raw = [&](unsigned i) {
        return RawBuf(slot, 1154, pool_rel, ReleaseCtx{&pool, i});
    };
    check_lifetime("Buffer", pool, make_buffer);
    check_lifetime("RawBuf", pool, make_raw);
    if (check_only) { return 0; }

    constexpr size_t N = 20'000'000;
    constexpr size_t rounds = 10;
    std::vector<Buffer> inflight(64);
    std::vector<RawBuf> rawfl(64);

    auto erased = [&](size_t k) {
        const unsigned i = static_cast<unsigned>(k & 63);
        inflight[i] = make_buffer(i);
        escape_memory(&inflight[i]);
        inflight[i] = Buffer{};
        escape_memory(pool.free_.data());
        pool.free_.clear();
    };
    auto raw = [&](size_t k) {
        const unsigned i = static_cast<unsigned>(k & 63);
        rawfl[i] = make_raw(i);
        escape_memory(&rawfl[i]);
        rawfl[i] = RawBuf{};
        escape_memory(pool.free_.data());
        pool.free_.clear();
    };

    std::printf("Compiler: %s\n", __VERSION__);
#ifdef __GLIBCXX__
    std::printf("libstdc++ date: %ld\n", static_cast<long>(__GLIBCXX__));
#endif
    std::printf("sizeof(Buffer)=%zu sizeof(RawBuf)=%zu\n", sizeof(Buffer), sizeof(RawBuf));
    std::printf("%zu rounds, %zu lifetimes/owner/round; alternating order\n", rounds, N);
    std::puts("Warmup: 1000000 lifetimes per owner (excluded)");
    (void)bench(1'000'000, erased);
    (void)bench(1'000'000, raw);

    std::array<double, rounds> erased_ns{}, raw_ns{}, differences{};
    for (size_t round = 0; round < rounds; ++round) {
        Sample e{}, r{};
        if (round % 2 == 0) {
            e = bench(N, erased);
            r = bench(N, raw);
        } else {
            r = bench(N, raw);
            e = bench(N, erased);
        }
        erased_ns[round] = e.ns;
        raw_ns[round] = r.ns;
        differences[round] = e.ns - r.ns;
        std::printf("round=%zu order=%s Buffer_ns=%.3f RawBuf_ns=%.3f "
                    "Buffer_new_calls=%zu RawBuf_new_calls=%zu\n",
                    round + 1, round % 2 == 0 ? "Buffer,RawBuf" : "RawBuf,Buffer",
                    e.ns, r.ns, e.new_calls, r.new_calls);
    }
    summarize("Buffer / std::function", erased_ns);
    summarize("RawBuf / function pointer", raw_ns);
    summarize("Paired difference (Buffer - Raw)", differences);
    std::puts("Whole-owner loop measurements, including compiler barriers and release work.");
    std::puts("new_calls counts intercepted ordinary scalar operator new calls only.");
    std::puts("No production-overhead percentage or per-GiB extrapolation is inferred.");
}
