//
// Created by benny on 6/13/26.
//

#include "bench.hpp"

#include "bench/latency.hpp"
#include "core/push.hpp"
#include "transport/tcp_server.hpp"
#include "util/constants.hpp"
#include "util/socket_fd.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>


namespace {

void print_bench_usage() {
    std::cerr << "Usage:\n"
              << "  embr bench --role send --file PATH [--port PORT]\n"
              << "  embr bench --role recv --host IP [--port PORT]\n"
              << "  [--sndbuf-bytes N] [--warmup N] [--runs-short N]\n"
              << "  --role send         listen and serve file for bench receiver\n"
              << "  --role recv         connect, timestamp and report percentiles\n"
              << "  --file PATH         file to push (send only)\n"
              << "  --port PORT         data port (default: " << EMBR_PORT << ")\n"
              << "  --sndbuf-bytes N    pin SO_SNDBUF on sender (0 = autotuned baseline)\n"
              << "  --warmup N          warmup runs excluded from stats (default: 5)\n"
              << "  --runs-short N      short loop runs (default: 1000)\n"
              << "  --runs-full N       full loop runs (default: 50)\n";
}

// Send: loop accepting connections and serving the file
// kill with SIGINT when bench is done
int run_send(const std::string& file_path, uint16_t port) {
    FileMeta file_meta = precompute_meta(file_path);
    SocketFd listen_fd = tcp_listen(port);
    std::cout << "[bench/send] listening on port " << port << "\n"
              << "[bench/send] file=" << file_meta.file_name
              << " size=" << file_meta.file_size
              << " chunks=" << file_meta.chunk_count << "\n"
              << "[bench/send] ready - start receiver\n";

    while (true) {
        auto conn = tcp_accept(listen_fd.get());
        // open fresh fd per connection, sendfile advance the offset internally
        SocketFd file_fd{::open(file_path.c_str(), O_RDONLY)};
        if (file_fd.get() < 0) {
            std::cerr << "[bench/send] open failed: " << std::strerror(errno) << "\n";
            continue;
        }

        try {
            run_push(*conn, std::move(file_fd), file_meta);
        } catch (const std::exception& ex) {
            std::cerr << "[bench/send] run_push ended: " << ex.what() << "\n";
        }
    }
}

// Recv: run short + full loops, print results
int run_recv(const BenchConfig& cfg) {
    std::cout << "[bench/recv] host=" << cfg.sender_port
              << " port=" << cfg.sender_port << "\n"
              << "[bench/recv] sndbuf_bytes="
              << (cfg.sndbuf_bytes == 0
                    ? std::string("autotuned (baseline)")
                    : std::to_string(cfg.sndbuf_bytes))
              << "\n"
              << "[bench/recv] warmup=" << cfg.warmup
              << " runs_short=" << cfg.runs_short
              << " runs_full=" << cfg.runs_full << "\n\n";

    // short loop - connection-setup + TTFC
    std::cout << "running short loop ("
              << cfg.runs_short << " + " << cfg.warmup << " warmup)...\n";
    auto short_r = run_short_loop(cfg);
    print_short_results(short_r, cfg.runs_short);

    // full loop - completion distribution + hash-verify baseline
    std::cout << "\nrunning full loop ("
              << cfg.runs_full << " + " << cfg.warmup << " warmup)...\n";
    auto full_r = run_full_loop(cfg);
    print_full_results(full_r, cfg.runs_full);
    return 0;
}

} // namespace

int run_bench_cli(int argc, char* argv[]) {
    if (argc < 2) {
        print_bench_usage();
        return 1;
    }

    std::string role;
    std::string file_path;
    std::string host;
    uint16_t port = EMBR_PORT;
    int sndbuf = 0;
    uint32_t warmup = 5;
    uint32_t runs_short = 1000;
    uint32_t runs_full = 50;

    for (int i = 1; i < argc; i++) {
        const std::string flag = argv[i];
        if (flag == "--role" && i + 1 < argc) {
            role = argv[++i];
        } else if (flag == "--file" && i + 1 < argc) {
            file_path = argv[++i];
        } else if (flag == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (flag == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (flag == "--sndbuf-bytes" && i + 1 < argc) {
            sndbuf = std::stoi(argv[++i]);
        } else if (flag == "--warmup" && i + 1 < argc) {
            warmup = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (flag == "--runs-short" && i + 1 < argc) {
            runs_short = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (flag == "--runs-full" && i + 1 < argc) {
            runs_full = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (flag == "--help") {
            print_bench_usage();
            return 0;
        } else {
            std::cerr << "embr bench: unknown flag: " << flag << "\n";
            print_bench_usage();
            return 1;
        }
    }

    if (role.empty()) {
        std::cerr << "embr bench: --role send/recv required\n";
        print_bench_usage();
        return 1;
    }

    try {
        if (role == "send") {
            if (file_path.empty()) {
                std::cerr << "embr bench: --file required for send\n";
                return 1;
            }
            return run_send(file_path, port);

        } else if (role == "recv") {
            if (host.empty()) {
                std::cerr << "embr bench: --host required for recv\n";
                return 1;
            }
            BenchConfig cfg{
                .sender_host = host,
                .sender_port = port,
                .sndbuf_bytes = sndbuf,
                .warmup = warmup,
                .runs_short = runs_short,
                .runs_full = runs_full,
            };
            return run_recv(cfg);

        } else {
            std::cerr << "embr bench: unknown role: " << role
                      << " (expected send/recv)\n";
            return 1;
        }

    } catch (const std::exception& ex) {
        std::cerr << "embr bench: " << ex.what() << "\n";
        return 1;
    }
}
