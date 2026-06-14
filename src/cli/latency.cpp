//
// Created by benny on 6/13/26.
//

#include "latency.hpp"
#include "core/hash.hpp"
#include "core/protocol.hpp"
#include "transport/tcp_server.hpp"
#include "util/constants.hpp"
#include "util/socket_fd.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <ctime>
#include <vector>

static uint64_t now_ns() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Open, tune and connect a raw socket, wraps into TcpTransport via tcp_from_fd
// actual data plane under measurement is TcpTransport::senf_file / recv_file
static int make_connection_fd(const std::string& host, uint16_t port, int sndbuf_bytes) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(
            std::string("bench: socket failed: ") + std::strerror(errno));
    }

    // TCP_NODELAY same as production
    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // SO_SNDBUF: only if sweep point provided, 0 = autotuned baseline
    if (sndbuf_bytes > 0) {
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf_bytes, sizeof(sndbuf_bytes));
        // verify effective, kernel doubles and clamps at net.core.wmem_max
        int effective = 0;
        socklen_t optlen = sizeof(effective);
        ::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &effective, &optlen);
        std::printf("[bench] SO_SNDBUF requested=%d effective=%d\n",
                    sndbuf_bytes, effective);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr.s_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("bench: invalid sender address: " + host);
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error(
            std::string("bench: connect failed: ") + std::strerror(errno));
    }
    return fd;
}

ShortLoopResults run_short_loop(const BenchConfig& cfg) {
    ShortLoopResults results;
    results.peer_connect_ns.reserve(cfg.runs_short);
    results.ttfc_ns.reserve(cfg.runs_short);

    for (uint32_t run = 0; run < cfg.runs_short + cfg.warmup; run++) {
        const bool is_warmup = (run < cfg.warmup);

        // time tcp_connect only, tracker resolve is a separate number
        const uint64_t t_connect_start = now_ns();
        int raw_fd = make_connection_fd(cfg.sender_host, cfg.sender_port, cfg.sndbuf_bytes);
        const uint64_t t_connect_end = now_ns();
        auto transport = tcp_from_fd(SocketFd{raw_fd});

        // pull side initiates
        send_msg(*transport, make_handshake(HandshakePayload{""}));
        Message meta_msg = recv_msg(*transport);
        if (meta_msg.type != MsgType::FILE_META) {
            throw std::runtime_error("bench short: expected FILE_META");
        }
        FileMeta meta = parse_filemeta(meta_msg);

        // populate file info from first non-warmup run
        if (!is_warmup && results.file_size == 0) {
            results.file_size = meta.file_size;
            results.chunk_count = meta.chunk_count;
        }

        // request exactly one chunk then signal end-of-requests
        send_msg(*transport, make_chunk_req(ChunkReq{0}));
        send_msg(*transport, make_complete());

        // TTFC: time from request sent to first CHUNK_HDR bytes received
        const uint64_t t_req_sent = now_ns();
        Message hdr_msg = recv_msg(*transport);
        const uint64_t t_first_chunk = now_ns();

        if (hdr_msg.type != MsgType::CHUNK_HDR) {
            throw std::runtime_error("bench short: expected CHUNK_HDR");
        }

        // abort, drop connection after first header
        // sender sees EPIPE/ECONNRESET (SIGPIPE ignored in main.cpp)

        if (!is_warmup) {
            results.peer_connect_ns.push_back(t_connect_end - t_connect_start);
            results.ttfc_ns.push_back(t_first_chunk - t_req_sent);
        }
    }
    return results;
}

FullLoopResults run_full_loop(const BenchConfig& cfg) {
    FullLoopResults results;
    results.completion_ns.reserve(cfg.runs_full);

    // output file: pre-allocate once, reuse across runs
    char tmp1[] = "/tmp/embr_bench_XXXXXX";
    int out_fd = ::mkstemp(tmp1);
    if (out_fd < 0) {
        throw std::runtime_error("bench full: mkstemp() failed");
    }
    ::unlink(tmp1); // unlink immediately

    for (uint32_t run = 0; run < cfg.runs_full + cfg.warmup; run++) {
        const bool is_warmup = (run < cfg.warmup);
        int raw_fd = make_connection_fd(cfg.sender_host, cfg.sender_port, cfg.sndbuf_bytes);
        auto transport = tcp_from_fd(SocketFd{raw_fd});
        send_msg(*transport, make_handshake(HandshakePayload{""}));
        Message meta_msg = recv_msg(*transport);
        if (meta_msg.type != MsgType::FILE_META) {
            throw std::runtime_error("bench full: expected FILE_META");
        }
        FileMeta meta = parse_filemeta(meta_msg);

        // file_size from FILE_META
        if (!is_warmup && results.file_size == 0) {
            results.file_size = meta.file_size;
            results.chunk_count = meta.chunk_count;
        }

        ::ftruncate(out_fd, static_cast<off_t>(meta.file_size));

        // flood all CHUNK_REQs then signal end-of-requests
        for (uint32_t i = 0; i < meta.chunk_count; i++) {
            send_msg(*transport, make_chunk_req(ChunkReq{i}));
        }
        send_msg(*transport, make_complete());

        const uint64_t t_transfer_start = now_ns();

        for (uint32_t i = 0; i < meta.chunk_count; i++) {
            Message hdr_msg = recv_msg(*transport);
            if (hdr_msg.type != MsgType::CHUNK_HDR) {
                throw std::runtime_error("bench full: expected CHUNK_HDR at chunk " +
                    std::to_string(i));
            }

            const uint64_t offset = static_cast<uint64_t>(i) * CHUNK_SIZE;
            const uint64_t chunk_len = std::min(static_cast<uint64_t>(CHUNK_SIZE),
                                                meta.file_size - offset);
            transport->recv_file(out_fd, offset, chunk_len);

            // per-chunk hash-verify, recv side
            const uint64_t t_verify_start = now_ns();
            void* mapped = ::mmap(nullptr, static_cast<size_t>(chunk_len),
                                  PROT_READ, MAP_SHARED, out_fd,
                                  static_cast<off_t>(offset));
            if (mapped == MAP_FAILED) {
                throw std::runtime_error(
                    "bench full: mmap for verify failed at chunk " +
                    std::to_string(i) + ": " + std::strerror(errno));
            }
            auto computed = sha256_buf(static_cast<const uint8_t*>(mapped),
                                       static_cast<size_t>(chunk_len));
            ::munmap(mapped, static_cast<size_t>(chunk_len));
            const uint64_t t_verify_end = now_ns();

            if (computed != meta.chunk_hashes[i]) {
                throw std::runtime_error(
                    "bench full: hash mismatch at chunk " + std::to_string(i));
            }

            if (!is_warmup) {
                results.verify_ns.record(t_verify_end - t_verify_start);
            }
        }

        const uint64_t t_transfer_end = now_ns();

        if (!is_warmup) {
            results.completion_ns.push_back(t_transfer_end - t_transfer_start);
        }
    }

    ::close(out_fd);
    return results;
}

void print_short_results(const ShortLoopResults& results, uint32_t runs) {
    auto sorted_pct = [](std::vector<uint64_t> v, double p) -> uint64_t {
        if (v.empty()) { return 0; }
        std::sort(v.begin(), v.end());
        const size_t idx = static_cast<size_t>(p * static_cast<double>(v.size()));
        return v[std::min(idx, v.size() - 1)];
    };
    auto us = [](uint64_t ns) { return static_cast<double>(ns) / 1000.0; };

    std::printf("\n--- Peer connect(N=%u) ---\n", runs);
    std::printf("  p50 %8.1f µs\n", us(sorted_pct(results.peer_connect_ns, 0.50)));
    std::printf("  p99 %8.1f µs\n", us(sorted_pct(results.peer_connect_ns, 0.99)));
    std::printf("  max %8.1f µs\n", us(*std::max_element(results.peer_connect_ns.begin(),
                                                         results.peer_connect_ns.end())));
    std::printf("\n--- Time-to-first-chunk (N=%u) ---\n", runs);
    std::printf("  p50 %8.1f µs\n", us(sorted_pct(results.ttfc_ns, 0.50)));
    std::printf("  p99 %8.1f µs\n", us(sorted_pct(results.ttfc_ns, 0.99)));
    std::printf("  max %8.1f µs\n", us(*std::max_element(results.ttfc_ns.begin(),
                                                         results.ttfc_ns.end())));
}

void print_full_results(const FullLoopResults& results, uint32_t runs) {
    auto completion = results.completion_ns;
    std::sort(completion.begin(), completion.end());

    auto pct = [&](double p) -> uint64_t {
        if (completion.empty()) { return 0; }
        const size_t idx = static_cast<size_t>(p * static_cast<double>(completion.size()));
        return completion[std::min(idx, completion.size() - 1)];
    };

    auto ms = [](uint64_t ns) {
        return static_cast<double>(ns) / 1e6;
    };
    auto gbs = [&](uint64_t ns) -> double {
        if (ns == 0 || results.file_size == 0) { return 0.0; }
        return static_cast<double>(results.file_size) /
               (static_cast<double>(ns) / 1e9) / (1024.0 * 1024.0 * 1024.0);
    };

    std::printf("\n--- Completion time (N=%u, file=%.2f GB) ---\n", runs,
                static_cast<double>(results.file_size) / (1024.0 * 1024.0 * 1024.0));
    std::printf("  p50 %8.1f ms %.2f GB/s\n", ms(pct(0.50)), gbs(pct(0.50)));
    std::printf("  p90 %8.1f ms %.2f GB/s\n", ms(pct(0.90)), gbs(pct(0.90)));
    std::printf("  max %8.1f ms %.2f GB/s\n", ms(completion.back()), gbs(completion.back()));
    std::printf("  jitter (max-p50) %.1f ms\n", ms(completion.back()) - ms(pct(0.50)));

    std::printf("\n--- Hash-verify per chunk (~%zu samples, baseline) ---\n", results.verify_ns.total);
    std::printf("  p50 %6.2f µs\n", static_cast<double>(results.verify_ns.p50()) / 1000.0);
    std::printf("  p99 %6.2f µs\n", static_cast<double>(results.verify_ns.p99()) / 1000.0);
    std::printf("  mean %6.2f µs\n", static_cast<double>(results.verify_ns.mean()) / 1000.0);
}
