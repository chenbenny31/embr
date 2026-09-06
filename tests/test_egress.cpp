//
// Created by benny on 9/5/26.
//

#include "transport/quic_egress.hpp"
#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <ctime>
#include <set>
#include <thread>
#include <stdexcept>
#include <vector>

namespace {

constexpr size_t kSlot = 1350;

uint64_t now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

struct UdpPair {
    int a{-1}, b{-1};
    ~UdpPair() { if (a >= 0) ::close(a); if (b >= 0) ::close(b); }
};

// two loopback UDP sockets connected to each other, so send() semantics apply
UdpPair make_pair() {
    UdpPair p;
    sockaddr_in addr[2]{};
    int fds[2];
    for (int i = 0; i < 2; ++i) {
        fds[i] = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fds[i] < 0) { throw std::runtime_error("socket() failed"); }
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind_addr.sin_port = 0;
        if (::bind(fds[i], reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
            throw std::runtime_error("bind() failed");
        }
        socklen_t len = sizeof(addr[i]);
        ::getsockname(fds[i], reinterpret_cast<sockaddr*>(&addr[i]), &len);
    }
    if (::connect(fds[0], reinterpret_cast<sockaddr*>(&addr[1]), sizeof(addr[1])) < 0 ||
        ::connect(fds[1], reinterpret_cast<sockaddr*>(&addr[0]), sizeof(addr[0])) < 0) {
        throw std::runtime_error("connect() failed");
    }
    timeval tv{2, 0};
    ::setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const int rcv = 4 * 1024 * 1024; // the test batches sends; a real peer drains as it goes
    ::setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    p.a = fds[0]; p.b = fds[1];
    return p;
}

size_t len_for(unsigned k) { return 8 + (k * 37) % (kSlot - 8); } // varied, never above the slot

void fill(uint8_t* p, unsigned k, size_t n) {
    for (size_t i = 0; i < n; ++i) { p[i] = static_cast<uint8_t>((k + i) & 0xff); }
    p[0] = static_cast<uint8_t>(k >> 24); p[1] = static_cast<uint8_t>(k >> 16);
    p[2] = static_cast<uint8_t>(k >> 8);  p[3] = static_cast<uint8_t>(k);
}

unsigned key_of(const uint8_t* p) {
    return (static_cast<unsigned>(p[0]) << 24) | (static_cast<unsigned>(p[1]) << 16) |
           (static_cast<unsigned>(p[2]) << 8) | static_cast<unsigned>(p[3]);
}

// receive exactly n datagrams; returns the set of keys seen, checking each body
std::set<unsigned> receive_all(int fd, unsigned n) {
    std::set<unsigned> seen;
    std::vector<uint8_t> buf(kSlot + 1);
    while (seen.size() < n) {
        const ssize_t got = ::recv(fd, buf.data(), buf.size(), 0);
        if (got < 0) { break; } // timeout
        const unsigned k = key_of(buf.data());
        EXPECT_EQ(static_cast<size_t>(got), len_for(k)) << "datagram " << k;
        for (size_t i = 4; i < static_cast<size_t>(got); ++i) {
            if (buf[i] != static_cast<uint8_t>((k + i) & 0xff)) { ADD_FAILURE() << "body " << k; break; }
        }
        seen.insert(k);
    }
    return seen;
}

} // namespace

// more datagrams than slots: reserve() must block on completions and recycle
TEST(ZcEgress, SendsAllDatagramsAndReturnsSlots) {
    auto pair = make_pair();
    constexpr unsigned kSlots = 16, kN = 200;
    ZcEgress eg(pair.a, kSlot, kSlots);

    std::set<unsigned> seen;
    std::jthread receiver([&] { seen = receive_all(pair.b, kN); });

    for (unsigned k = 0; k < kN; ++k) {
        uint8_t* p = eg.reserve();
        const size_t n = len_for(k);
        fill(p, k, n);
        ASSERT_TRUE(eg.commit(n)) << "errno " << eg.error();
        if (k % 10 == 9) { eg.flush(); }
    }
    eg.flush();

    receiver.join();
    EXPECT_EQ(seen.size(), kN);

    EXPECT_TRUE(eg.drain(now_ns() + 2'000'000'000ULL));
    EXPECT_EQ(eg.inflight(), 0u);
    EXPECT_EQ(eg.error(), 0);

    const auto& st = eg.stats();
    EXPECT_EQ(st.sends, kN);
    EXPECT_EQ(st.notifs, kN);         // every accepted SEND_ZC gets its NOTIF
    EXPECT_LE(st.copied, st.notifs);  // loopback usually copies; that is reported, not hidden
    EXPECT_EQ(st.errors, 0u);
}

TEST(ZcEgress, ReserveWithoutCommitReusesTheSlot) {
    auto pair = make_pair();
    ZcEgress eg(pair.a, kSlot, 4);
    uint8_t* p1 = eg.reserve();
    uint8_t* p2 = eg.reserve();
    EXPECT_EQ(p1, p2);
    EXPECT_EQ(eg.inflight(), 0u);
    fill(p1, 7, len_for(7));
    ASSERT_TRUE(eg.commit(len_for(7)));
    EXPECT_EQ(eg.inflight(), 1u);
    eg.flush();
    EXPECT_EQ(receive_all(pair.b, 1).count(7), 1u);
    EXPECT_TRUE(eg.drain(now_ns() + 2'000'000'000ULL));
    EXPECT_EQ(eg.inflight(), 0u);
}

TEST(ZcEgress, CommitRejectsBadLengths) {
    auto pair = make_pair();
    ZcEgress eg(pair.a, kSlot, 2);
    EXPECT_THROW(eg.commit(1), std::logic_error);      // nothing reserved
    eg.reserve();
    EXPECT_THROW(eg.commit(0), std::invalid_argument);
    EXPECT_THROW(eg.commit(kSlot + 1), std::invalid_argument);
}

// unflushed sends must still go out and complete when the pool is destroyed
TEST(ZcEgress, DestructorSubmitsAndDrains) {
    auto pair = make_pair();
    constexpr unsigned kN = 5;
    {
        ZcEgress eg(pair.a, kSlot, 8);
        for (unsigned k = 0; k < kN; ++k) {
            uint8_t* p = eg.reserve();
            fill(p, k, len_for(k));
            ASSERT_TRUE(eg.commit(len_for(k)));
        }
    } // no flush: the destructor drains
    EXPECT_EQ(receive_all(pair.b, kN).size(), kN);
}
