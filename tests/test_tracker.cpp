//
// Created by benny on 4/27/26.
//

// tests/test_tracker.cpp

#include "tracker/token_store.hpp"
#include "tracker/tracker_handlers.hpp"
#include "tracker/tracker_server.hpp"
#include "tracker/tracker_client.hpp"
#include "util/constants.hpp"
#include "util/token.hpp"
#include <httplib.h>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <gtest/gtest.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

namespace {

// Starts tracker in a background jthread, returns stop handle.
// Waits briefly for server to bind before returning.
// tests/test_tracker.cpp — replace TrackerHandle
struct TrackerHandle {
    httplib::Server        server;
    TokenStore             store{std::chrono::minutes(10)};
    std::jthread           thread;

    TrackerHandle(uint16_t port) { // v0.6: bypass run_tracker due to SIGINT simulation
        // wire handlers directly — no signal handler, no sweeper
        server.Post("/register", [this](const httplib::Request& req,
                                        httplib::Response& res) {
            handle_register(req, res, store);
        });
        server.Get("/resolve/:token", [this](const httplib::Request& req,
                                              httplib::Response& res) {
            handle_resolve(req, res, store);
        });
        server.Post("/unregister/:token", [this](const httplib::Request& req,
                                                  httplib::Response& res) {
            handle_unregister(req, res, store);
        });

        thread = std::jthread([this, port](std::stop_token stop) {
            server.listen("127.0.0.1", port);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~TrackerHandle() {
        server.stop();  // unblocks server.listen() → thread exits → jthread joins
    }
};

const std::string TEST_TRACKER_URL = "http://127.0.0.1:19009";
const uint16_t    TEST_TRACKER_PORT = 19009;  // test-only port, avoids conflict with 10009
const std::string VALID_TOKEN  = "3a7b4e2f1c9d8a6b";  // 16 lowercase hex chars
const std::string SHORT_TOKEN  = "3a7b4e2f";           // 8 chars — invalid
const std::string BAD_TOKEN    = "3a7b4e2f1c9dXXXX";  // non-hex chars — invalid

} // namespace

// ── TokenStore unit tests ─────────────────────────────────────────────────────

TEST(TokenStore, UpsertAndResolve) {
    TokenStore store(std::chrono::minutes(10));
    store.upsert(VALID_TOKEN, Record{"1.2.3.4", 4747, {}});
    auto record = store.resolve(VALID_TOKEN);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->sender_ip,   "1.2.3.4");
    EXPECT_EQ(record->sender_port, 4747u);
}

TEST(TokenStore, UpsertOverwrites) {
    TokenStore store(std::chrono::minutes(10));
    store.upsert(VALID_TOKEN, Record{"1.2.3.4", 4747, {}});
    store.upsert(VALID_TOKEN, Record{"5.6.7.8", 9000, {}});
    auto record = store.resolve(VALID_TOKEN);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->sender_ip,   "5.6.7.8");
    EXPECT_EQ(record->sender_port, 9000u);
}

TEST(TokenStore, ResolveReturnsNulloptIfNotFound) {
    TokenStore store(std::chrono::minutes(10));
    auto record = store.resolve(VALID_TOKEN);
    EXPECT_FALSE(record.has_value());
}

TEST(TokenStore, RemoveReturnsTrueIfExisted) {
    TokenStore store(std::chrono::minutes(10));
    store.upsert(VALID_TOKEN, Record{"1.2.3.4", 4747, {}});
    EXPECT_TRUE(store.remove(VALID_TOKEN));
    EXPECT_FALSE(store.resolve(VALID_TOKEN).has_value());
}

TEST(TokenStore, RemoveReturnsFalseIfAbsent) {
    TokenStore store(std::chrono::minutes(10));
    EXPECT_FALSE(store.remove(VALID_TOKEN));
}

TEST(TokenStore, TTLEvictsExpiredOnResolve) {
    TokenStore store(std::chrono::minutes(0));  // TTL=0 — expires immediately
    store.upsert(VALID_TOKEN, Record{"1.2.3.4", 4747, {}});
    // sleep briefly so registered_at < now - ttl
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto record = store.resolve(VALID_TOKEN);
    EXPECT_FALSE(record.has_value());
}

TEST(TokenStore, SweepExpiredReturnsCount) {
    TokenStore store(std::chrono::minutes(0));
    store.upsert("aaaaaaaaaaaaaaaa", Record{"1.2.3.4", 4747, {}});
    store.upsert("bbbbbbbbbbbbbbbb", Record{"2.3.4.5", 4748, {}});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(store.sweep_expired(), 2u);
}

TEST(TokenStore, ConcurrentUpsertAndResolve) {
    TokenStore store(std::chrono::minutes(10));
    std::jthread writer([&store]() {
        for (int i = 0; i < 100; ++i) {
            store.upsert(VALID_TOKEN, Record{"1.2.3.4", 4747, {}});
        }
    });
    std::jthread reader([&store]() {
        for (int i = 0; i < 100; ++i) {
            store.resolve(VALID_TOKEN);  // must not crash
        }
    });
}

// ── valid_token unit tests ────────────────────────────────────────────────────

TEST(TrackerHandlers, ValidToken_AcceptsCorrect) {
    EXPECT_TRUE(valid_token(VALID_TOKEN));
}

TEST(TrackerHandlers, ValidToken_RejectsTooShort) {
    EXPECT_FALSE(valid_token(SHORT_TOKEN));
}

TEST(TrackerHandlers, ValidToken_RejectsNonHex) {
    EXPECT_FALSE(valid_token(BAD_TOKEN));
}

TEST(TrackerHandlers, ValidToken_RejectsUppercase) {
    EXPECT_FALSE(valid_token("3A7B4E2F1C9D8A6B"));  // uppercase hex — invalid
}

TEST(TrackerHandlers, ValidToken_RejectsEmpty) {
    EXPECT_FALSE(valid_token(""));
}

// ── Integration tests — full HTTP round-trip ──────────────────────────────────

TEST(TrackerIntegration, RegisterAndResolve) {
    TrackerHandle tracker(TEST_TRACKER_PORT);

    tracker_register(TEST_TRACKER_URL, VALID_TOKEN, 4747);

    auto [sender_ip, sender_port] = tracker_resolve(TEST_TRACKER_URL, VALID_TOKEN);
    EXPECT_FALSE(sender_ip.empty());
    EXPECT_EQ(sender_port, 4747u);

    tracker_unregister(TEST_TRACKER_URL, VALID_TOKEN);
}

TEST(TrackerIntegration, ResolveUnknownTokenThrows) {
    TrackerHandle tracker(TEST_TRACKER_PORT);
    EXPECT_THROW(tracker_resolve(TEST_TRACKER_URL, "0000000000000000"),
                 std::runtime_error);
}

TEST(TrackerIntegration, UnregisterIsIdempotent) {
    TrackerHandle tracker(TEST_TRACKER_PORT);
    tracker_register(TEST_TRACKER_URL, VALID_TOKEN, 4747);
    tracker_unregister(TEST_TRACKER_URL, VALID_TOKEN);
    tracker_unregister(TEST_TRACKER_URL, VALID_TOKEN);  // second call — must not throw
}

TEST(TrackerIntegration, UpsertUpdatesRecord) {
    TrackerHandle tracker(TEST_TRACKER_PORT);
    tracker_register(TEST_TRACKER_URL, VALID_TOKEN, 4747);
    tracker_register(TEST_TRACKER_URL, VALID_TOKEN, 9000);  // re-register same token

    auto [sender_ip, sender_port] = tracker_resolve(TEST_TRACKER_URL, VALID_TOKEN);
    EXPECT_EQ(sender_port, 9000u);  // last-writer-wins

    tracker_unregister(TEST_TRACKER_URL, VALID_TOKEN);
}

// ── Token: content id, optional certificate cert_hash ──────────────────────

TEST(Token, ParsesContentHashOnly) {
    const auto token = parse_token("fc1d2e8bb7f4f64b");
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(token->content_hash, "fc1d2e8bb7f4f64b");
    EXPECT_TRUE(token->cert_hash.empty());
}

TEST(Token, ParsesContentHashAndCertHash) {
    const auto token = parse_token("fc1d2e8bb7f4f64b-7da37c097be61c69973ea4e576880420");
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(token->content_hash, "fc1d2e8bb7f4f64b");
    EXPECT_EQ(token->cert_hash, "7da37c097be61c69973ea4e576880420");
}

TEST(Token, RejectsMalformed) {
    for (const char* bad : {"", "10.0.0.5", "fc1d2e8bb7f4f64", "FC1D2E8BB7F4F64B",
                            "fc1d2e8bb7f4f64b-", "fc1d2e8bb7f4f64b-7da37c09",
                            "fc1d2e8bb7f4f64b_7da37c097be61c69973ea4e576880420",
                            "fc1d2e8bb7f4f64b-7da37c097be61c69973ea4e57688042g"}) {
        EXPECT_FALSE(parse_token(bad).has_value()) << "accepted '" << bad << "'";
    }
}

TEST(Token, FormatAddsCertHashOnlyWhenThereIsACertificate) {
    const std::string digest = "7da37c097be61c69973ea4e57688042039c90ad18d8c629001355918db401faf";
    EXPECT_EQ(format_token("fc1d2e8bb7f4f64b", digest),
              "fc1d2e8bb7f4f64b-7da37c097be61c69973ea4e576880420");
    EXPECT_EQ(format_token("fc1d2e8bb7f4f64b", ""), "fc1d2e8bb7f4f64b");
    EXPECT_TRUE(parse_token(format_token("fc1d2e8bb7f4f64b", digest)).has_value());
}
