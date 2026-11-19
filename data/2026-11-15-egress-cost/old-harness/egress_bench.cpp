// send-only cost of embr's egress paths: same call pattern as QuicTransport, no QUIC, no receiver
// egress_bench MODE DST PORT PKT BURST GSO TOTAL_PKTS [RING_EXTRA_FLAGS_HEX]
//   MODE  sendmsg | usend (io_uring SEND, copies) | zc (embr's ZcEgress, SEND_ZC fixed buffer)
//   BURST packets written per flush (what one ACK frees);  GSO 0|1 (up to 16 packets per send)
#include "transport/quic_egress.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>
#include <sched.h>
#include <fstream>
#include <sstream>
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

unsigned g_ring_extra = 0;
static const size_t MAXPKT = 1350;
static unsigned SEGS = 16;

static double tv(const timeval& t) { return t.tv_sec + t.tv_usec / 1e6; }
// softirq seconds of one cpu from /proc/stat: IRQ_TIME_ACCOUNTING keeps this out of the task's stime
static double softirq_s(int cpu) {
    std::ifstream f("/proc/stat"); std::string line; const std::string key = "cpu" + std::to_string(cpu) + " ";
    while (std::getline(f, line)) {
        if (line.compare(0, key.size(), key) == 0) {
            std::istringstream is(line.substr(key.size())); unsigned long long v[8]{};
            for (auto& x : v) { is >> x; }
            return (v[5] + v[6]) / static_cast<double>(sysconf(_SC_CLK_TCK)); // irq + softirq
        }
    }
    return 0;
}
static double mono() { timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

int main(int argc, char** argv) {
    if (argc < 8) { fprintf(stderr, "usage\n"); return 2; }
    const std::string mode = argv[1];
    const char* dst = argv[2];
    const int port = atoi(argv[3]);
    const size_t pkt = static_cast<size_t>(atoi(argv[4]));
    const unsigned burst = static_cast<unsigned>(atoi(argv[5]));
    const bool gso = atoi(argv[6]) != 0;
    const uint64_t total = strtoull(argv[7], nullptr, 10);
    if (argc > 8) { g_ring_extra = static_cast<unsigned>(strtoul(argv[8], nullptr, 16)); }

    // sink on loopback: bound, never read; packets die at the socket after the full receive path
    int sink = -1;
    if (strncmp(dst, "127.", 4) == 0) {
        sink = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(port);
        inet_pton(AF_INET, dst, &sa.sin_addr);
        if (bind(sink, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) { perror("bind"); return 1; }
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(port);
    inet_pton(AF_INET, dst, &to.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0) { perror("connect"); return 1; }
    int sndbuf = 4 << 20; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    if (gso) {
        const int seg = static_cast<int>(pkt);
        if (setsockopt(fd, SOL_UDP, UDP_SEGMENT, &seg, sizeof(seg)) < 0) { perror("UDP_SEGMENT"); return 1; }
    }

    std::vector<uint8_t> src(1 << 20, 0xA5); // stands in for the encryptor's output
    size_t src_off = 0;
    if (const char* e = getenv("SEGS")) { SEGS = static_cast<unsigned>(atoi(e)); }
    const unsigned per_send = gso ? SEGS : 1;
    const size_t slot = per_send * MAXPKT;

    std::unique_ptr<ZcEgress> zc;
    io_uring ring{};
    std::vector<uint8_t> slab;
    unsigned uslot = 0, uslots = 64;
    if (mode == "zc") {
        const unsigned nslots = getenv("SLOTS") ? static_cast<unsigned>(atoi(getenv("SLOTS"))) : (gso ? 8u : 64u);
        zc = std::make_unique<ZcEgress>(fd, slot, nslots);
        if (!zc->registered()) { fprintf(stderr, "not registered\n"); return 1; }
    } else if (mode == "usend") {
        if (io_uring_queue_init(128, &ring, IORING_SETUP_SUBMIT_ALL | g_ring_extra) < 0) { perror("ring"); return 1; }
        slab.resize(uslots * slot);
    } else {
        slab.resize(slot);
    }

    uint64_t sends = 0, errs = 0, enters = 0;
    rusage r0{}, r1{};
    const int cpu = sched_getcpu();
    const double q0 = softirq_s(cpu);
    getrusage(RUSAGE_SELF, &r0);
    const double t0 = mono();

    uint64_t done = 0;
    while (done < total) {
        uint8_t* base = nullptr; size_t len = 0; unsigned n = 0;
        auto send_batch = [&]() {
            if (n == 0) { return; }
            ++sends;
            if (zc) {
                if (!zc->commit(len)) { ++errs; }
            } else if (mode == "usend") {
                io_uring_sqe* sqe = io_uring_get_sqe(&ring);
                io_uring_prep_send(sqe, fd, base, len, 0);
                uslot = (uslot + 1) % uslots;
            } else {
                iovec iov{ base, len };
                msghdr msg{}; msg.msg_iov = &iov; msg.msg_iovlen = 1;
                if (sendmsg(fd, &msg, 0) < 0) { ++errs; }
                ++enters;
            }
            len = 0; n = 0;
        };
        for (unsigned b = 0; b < burst && done < total; ++b, ++done) {
            if (n == 0) {
                if (zc) { base = zc->reserve(); if (!base) { fprintf(stderr, "ring error %d\n", zc->error()); return 1; } }
                else if (mode == "usend") { base = slab.data() + uslot * slot; }
                else { base = slab.data(); }
            }
            std::memcpy(base + len, src.data() + src_off, pkt);
            src_off = (src_off + pkt) & ((1 << 20) - 1 - 2047);
            len += pkt; ++n;
            if (n >= per_send) { send_batch(); }
        }
        send_batch();
        // one submit per burst, as flush_packets() does
        if (zc) { zc->flush(); }
        else if (mode == "usend") {
            io_uring_submit(&ring); ++enters;
            io_uring_cqe* cqe = nullptr;
            while (io_uring_peek_cqe(&ring, &cqe) == 0 && cqe) { if (cqe->res < 0) { ++errs; } io_uring_cqe_seen(&ring, cqe); }
        }
    }
    if (zc) { zc->drain(static_cast<uint64_t>((mono() + 1.0) * 1e9)); }
    const double t1 = mono();
    getrusage(RUSAGE_SELF, &r1);
    const double q1 = softirq_s(cpu);

    uint64_t copied = 0, notifs = 0, zerrs = 0, fallback = 0;
    if (zc) { copied = zc->stats().copied; notifs = zc->stats().notifs; zerrs = zc->stats().errors; fallback = zc->stats().fallback; }
    const double us = tv(r1.ru_utime) - tv(r0.ru_utime), sy = tv(r1.ru_stime) - tv(r0.ru_stime);
    printf("mode=%s flags=%x pkt=%zu burst=%u gso=%d pkts=%llu sends=%llu wall=%.3f user=%.3f sys=%.3f "
           "softirq=%.2f ns_pkt: w=%.0f s=%.0f u=%.0f copied=%llu notifs=%llu errs=%llu fallback=%llu\n",
           mode.c_str(), g_ring_extra, pkt, burst, gso ? 1 : 0, (unsigned long long)total,
           (unsigned long long)sends, t1 - t0, us, sy, q1 - q0, (t1 - t0) * 1e9 / total, sy * 1e9 / total, us * 1e9 / total,
           (unsigned long long)copied, (unsigned long long)notifs,
           (unsigned long long)(errs + zerrs), (unsigned long long)fallback);
    if (sink >= 0) { close(sink); }
    return 0;
}
