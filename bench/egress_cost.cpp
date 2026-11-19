// Send-only cost of embr's two QUIC egress paths: QuicTransport's call pattern with no QUIC and
// no receiver, so the difference between two rows is the send path alone.
//
//   egress_cost MODE DST PKT BURST SEGS TOTAL [SLOTS]
//     MODE   sendmsg | zc (ZcEgress: io_uring SEND_ZC from the registered slab, as embr sends)
//     DST    IPv4 address, port 19001; a 127.x address gets a bound, never-read sink socket
//     PKT    bytes per packet
//     BURST  packets written before one flush (what one ACK frees in a transfer)
//     SEGS   packets per send: 1 is one datagram per send, more is one UDP GSO send
//     TOTAL  packets to send
//     SLOTS  ZcEgress slots; default is what QuicTransport uses (64, or 8 with GSO)
//
// bench/egress_cost.sh runs the sweep on a dummy device in a private network namespace.

#include "transport/quic_egress.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103 // libc headers older than the kernel feature
#endif

static double seconds(const timeval& t) { return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) / 1e6; }

static double mono() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// irq + softirq seconds of one cpu: with IRQ_TIME_ACCOUNTING the kernel keeps packet work done in
// softirq out of the task's system time, so getrusage alone under-reports a send on loopback
static double softirq_seconds(int cpu) {
    std::ifstream stat("/proc/stat");
    const std::string key = "cpu" + std::to_string(cpu) + " ";
    std::string line;
    while (std::getline(stat, line)) {
        if (line.compare(0, key.size(), key) != 0) { continue; }
        std::istringstream fields(line.substr(key.size()));
        unsigned long long v[8]{};
        for (auto& x : v) { fields >> x; }
        return static_cast<double>(v[5] + v[6]) / static_cast<double>(sysconf(_SC_CLK_TCK));
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr, "usage: egress_cost sendmsg|zc DST PKT BURST SEGS TOTAL [SLOTS]\n");
        return 2;
    }
    const std::string mode = argv[1];
    const char* dst = argv[2];
    const size_t pkt = static_cast<size_t>(std::atoi(argv[3]));
    const unsigned burst = static_cast<unsigned>(std::atoi(argv[4]));
    const unsigned segs = static_cast<unsigned>(std::atoi(argv[5]));
    const uint64_t total = std::strtoull(argv[6], nullptr, 10);
    if ((mode != "sendmsg" && mode != "zc") || pkt == 0 || burst == 0 || segs == 0 || pkt * segs > 65507) {
        std::fprintf(stderr, "bad arguments\n");
        return 2;
    }
    const unsigned slots = argc > 7 ? static_cast<unsigned>(std::atoi(argv[7])) : (segs > 1 ? 8u : 64u);
    const uint16_t port = 19001;

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (inet_pton(AF_INET, dst, &to.sin_addr) != 1) { std::fprintf(stderr, "bad address\n"); return 2; }

    // loopback delivers: the packets need a socket to die at, after the whole receive path
    int sink = -1;
    if (std::strncmp(dst, "127.", 4) == 0) {
        sink = socket(AF_INET, SOCK_DGRAM, 0);
        if (bind(sink, reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0) { std::perror("bind"); return 1; }
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0) { std::perror("connect"); return 1; }
    const int sndbuf = 4 << 20; // as QuicTransport asks
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    if (segs > 1) {
        const int seg = static_cast<int>(pkt);
        if (setsockopt(fd, SOL_UDP, UDP_SEGMENT, &seg, sizeof(seg)) < 0) { std::perror("UDP_SEGMENT"); return 1; }
    }

    const size_t slot = segs * pkt;
    std::unique_ptr<ZcEgress> zc;
    std::vector<uint8_t> batch;
    if (mode == "zc") {
        zc = std::make_unique<ZcEgress>(fd, slot, slots);
        if (!zc->registered()) { std::fprintf(stderr, "slab not registered: raise ulimit -l\n"); return 1; }
    } else {
        batch.resize(slot);
    }

    // stands in for the encryptor's output: each packet is written into the send buffer once,
    // as ngtcp2 writes it, on both paths
    std::vector<uint8_t> src(1 << 20, 0xA5);
    size_t src_off = 0;

    uint64_t sends = 0;
    uint64_t errors = 0;
    rusage r0{};
    rusage r1{};
    const int cpu = sched_getcpu();
    const double q0 = softirq_seconds(cpu);
    getrusage(RUSAGE_SELF, &r0);
    const double t0 = mono();

    uint64_t done = 0;
    while (done < total) {
        uint8_t* base = nullptr;
        size_t len = 0;
        unsigned n = 0;
        auto send_batch = [&] {
            if (n == 0) { return; }
            ++sends;
            if (zc) {
                if (!zc->commit(len)) { ++errors; }
            } else {
                iovec iov{ base, len };
                msghdr msg{};
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                if (sendmsg(fd, &msg, 0) < 0) { ++errors; }
            }
            len = 0;
            n = 0;
        };
        for (unsigned b = 0; b < burst && done < total; ++b, ++done) {
            if (n == 0) {
                base = zc ? zc->reserve() : batch.data();
                if (!base) { std::fprintf(stderr, "ring error %d\n", zc->error()); return 1; }
            }
            std::memcpy(base + len, src.data() + src_off, pkt);
            src_off = (src_off + pkt) & ((1u << 20) - 1 - 2047);
            len += pkt;
            if (++n >= segs) { send_batch(); }
        }
        send_batch();
        if (zc) { zc->flush(); } // one submit per burst, as QuicTransport::flush_packets()
    }
    if (zc) { zc->drain(static_cast<uint64_t>((mono() + 1.0) * 1e9)); }

    const double t1 = mono();
    getrusage(RUSAGE_SELF, &r1);
    const double q1 = softirq_seconds(cpu);

    const double user = seconds(r1.ru_utime) - seconds(r0.ru_utime);
    const double sys = seconds(r1.ru_stime) - seconds(r0.ru_stime);
    const ZcEgress::Stats zs = zc ? zc->stats() : ZcEgress::Stats{};
    std::printf("mode=%s pkt=%zu burst=%u segs=%u slots=%u pkts=%llu sends=%llu wall=%.4f user=%.4f sys=%.4f "
                "softirq=%.2f copied=%llu waits=%llu errors=%llu\n",
                mode.c_str(), pkt, burst, segs, zc ? slots : 0u, static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(sends), t1 - t0, user, sys, q1 - q0,
                static_cast<unsigned long long>(zs.copied), static_cast<unsigned long long>(zs.waits),
                static_cast<unsigned long long>(errors + zs.errors));
    if (sink >= 0) { close(sink); }
    return 0;
}
