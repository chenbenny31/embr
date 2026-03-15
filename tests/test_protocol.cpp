//
// Created by benny on 3/15/26.
//

#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "core/protocol.hpp"

// ── Mock transport — in-memory pipe, no sockets ───────────────────────────────

class MockTransport : public Transport {
    std::vector<uint8_t> buf_;
    size_t               read_pos_{};
public:
    ssize_t send(const uint8_t* buf, size_t len) override {
        buf_.insert(buf_.end(), buf, buf + len);
        return static_cast<ssize_t>(len);
    }
    ssize_t recv(uint8_t* buf, size_t len) override {
        size_t available = buf_.size() - read_pos_;
        if (available == 0) return 0;
        size_t to_copy = std::min(len, available);
        std::memcpy(buf, buf_.data() + read_pos_, to_copy);
        read_pos_ += to_copy;
        return static_cast<ssize_t>(to_copy);
    }

    // inject raw bytes to simulate incoming data
    void inject(const uint8_t* data, size_t len) {
        buf_.insert(buf_.end(), data, data + len);
    }
};

// ── FileMeta round-trip ───────────────────────────────────────────────────────

TEST(Protocol, FileMetaRoundTrip) {
    MockTransport t;
    FileMeta original{"testfile.iso", 3ULL * 1024 * 1024 * 1024}; // 3GB

    send_msg(t, make_filemeta(FileMeta{original}));
    Message received = recv_msg(t);

    ASSERT_EQ(received.type, MsgType::FILE_META);
    FileMeta parsed = parse_filemeta(received);

    EXPECT_EQ(parsed.filename,  original.filename);
    EXPECT_EQ(parsed.file_size, original.file_size);
}

TEST(Protocol, FileMetaEmptyFilename) {
    MockTransport t;
    FileMeta meta{"", 1024};

    send_msg(t, make_filemeta(FileMeta{meta}));
    Message received = recv_msg(t);
    FileMeta parsed = parse_filemeta(received);

    EXPECT_EQ(parsed.filename,  "");
    EXPECT_EQ(parsed.file_size, 1024ULL);
}

TEST(Protocol, FileMetaLongFilename) {
    MockTransport t;
    std::string long_name(255, 'a'); // max typical filename length
    FileMeta meta{long_name, 42};

    send_msg(t, make_filemeta(FileMeta{meta}));
    Message received = recv_msg(t);
    FileMeta parsed = parse_filemeta(received);

    EXPECT_EQ(parsed.filename,  long_name);
    EXPECT_EQ(parsed.file_size, 42ULL);
}

// ── COMPLETE ──────────────────────────────────────────────────────────────────

TEST(Protocol, CompleteHasNoPayload) {
    MockTransport t;
    send_msg(t, make_complete());
    Message received = recv_msg(t);

    EXPECT_EQ(received.type, MsgType::COMPLETE);
    EXPECT_FALSE(received.payload.valid());
}

// ── ERROR round-trip ──────────────────────────────────────────────────────────

TEST(Protocol, ErrorRoundTrip) {
    MockTransport t;
    std::string reason = "disk full";

    send_msg(t, make_error(std::string{reason}));
    Message received = recv_msg(t);

    ASSERT_EQ(received.type, MsgType::ERROR);
    EXPECT_EQ(parse_error(received), reason);
}

TEST(Protocol, ErrorEmptyReason) {
    MockTransport t;
    send_msg(t, make_error(std::string{""}));
    Message received = recv_msg(t);

    ASSERT_EQ(received.type, MsgType::ERROR);
    EXPECT_EQ(parse_error(received), "");
}

// ── recv_msg error handling ───────────────────────────────────────────────────

TEST(Protocol, WrongVersionThrows) {
    MockTransport t;

    // inject header with wrong version
    uint8_t hdr[HEADER_SIZE] = {0x99, static_cast<uint8_t>(MsgType::FILE_META), 0, 0, 0, 0};
    t.inject(hdr, HEADER_SIZE);

    EXPECT_THROW(recv_msg(t), std::runtime_error);
}

TEST(Protocol, UnknownMsgTypeThrows) {
    MockTransport t;

    // inject header with out-of-range type (0xFF)
    uint8_t hdr[HEADER_SIZE] = {PROTOCOL_VERSION, 0xFF, 0, 0, 0, 0};
    t.inject(hdr, HEADER_SIZE);

    EXPECT_THROW(recv_msg(t), std::runtime_error);
}

TEST(Protocol, OversizedPayloadThrows) {
    MockTransport t;

    // inject header claiming payload > MAX_PAYLOAD_SIZE
    uint8_t hdr[HEADER_SIZE];
    hdr[0] = PROTOCOL_VERSION;
    hdr[1] = static_cast<uint8_t>(MsgType::FILE_META);
    uint32_t huge = htonl(MAX_PAYLOAD_SIZE + 1);
    std::memcpy(hdr + 2, &huge, sizeof(huge));
    t.inject(hdr, HEADER_SIZE);

    EXPECT_THROW(recv_msg(t), std::runtime_error);
}

// ── parse wrong type throws ───────────────────────────────────────────────────

TEST(Protocol, ParseFileMetaWrongTypeThrows) {
    MockTransport t;
    send_msg(t, make_complete());
    Message received = recv_msg(t);

    EXPECT_THROW(parse_filemeta(received), std::runtime_error);
}

TEST(Protocol, ParseErrorWrongTypeThrows) {
    MockTransport t;
    send_msg(t, make_complete());
    Message received = recv_msg(t);

    EXPECT_THROW(parse_error(received), std::runtime_error);
}

// ── Buffer ────────────────────────────────────────────────────────────────────

TEST(Buffer, HeapPathValid) {
    Buffer buf(1024);
    EXPECT_TRUE(buf.valid());
    EXPECT_EQ(buf.size, 1024ULL);
    EXPECT_NE(buf.get(), nullptr);
}

TEST(Buffer, DefaultInvalid) {
    Buffer buf;
    EXPECT_FALSE(buf.valid());
}

TEST(Buffer, MoveTransfersOwnership) {
    Buffer a(1024);
    uint8_t* ptr = a.get();
    Buffer b = std::move(a);

    EXPECT_EQ(b.get(), ptr);     // b owns the memory
    EXPECT_FALSE(a.valid());     // a is empty after move
}

TEST(Buffer, MoveAssignment) {
    Buffer a(1024);
    Buffer b(512);
    uint8_t* ptr = a.get();
    b = std::move(a);

    EXPECT_EQ(b.get(), ptr);
    EXPECT_EQ(b.size, 1024ULL);
    EXPECT_FALSE(a.valid());
}

TEST(Buffer, ReleaseCallbackCalled) {
    bool released = false;
    {
        uint8_t raw[64];
        Buffer buf(raw, sizeof(raw), [&released](uint8_t*) { released = true; });
        EXPECT_TRUE(buf.valid());
    } // buf goes out of scope here
    EXPECT_TRUE(released);
}