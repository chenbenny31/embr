//
// Created by benny on 3/11/26.
//

#pragma once

#include <cstdint>
#include <cstring>
#include <cerrno>
#include <functional>
#include <memory>
#include <vector>
#include <stdexcept>
#include <string>
#include <utility>
#include <arpa/inet.h>
#include "../transport/transport.hpp"


inline constexpr uint8_t PROTOCOL_VERSION = 0x01;
inline constexpr size_t HEADER_SIZE = 6; // version(1) + type(1) + payload_len(4)
inline constexpr size_t FILE_SIZE_BYTES = sizeof(uint64_t); // 8 - [file_size:u64 BE]
inline constexpr size_t LEN_PREFIX_BYTES = sizeof(uint32_t); // 4 - [filename_len:u32 BE]
inline constexpr uint32_t MAX_PAYLOAD_SIZE = 64 * 1024 * 1024; // 64MB, guard against malformed headers

inline constexpr size_t READ_BUF_SIZE = 1024 * 1024; // 1MB - v0.1 I/O buffer
inline constexpr size_t CHUNK_SIZE = 16 * 1024 * 1024; // 16MB - v0.2 chunk boundary

enum class MsgType : uint8_t {
    INVALID = 0x00, // sentinel for zero-value init
    HANDSHAKE = 0x01, // v0.2: HANDSHAKE introduced for tracker + token resolution
    FILE_META = 0x02, // v0.1: connections start directly with FILE_META
    CHUNK_REQ = 0x03,
    CHUNK_DATA = 0x04,
    RESUME = 0x05,
    COMPLETE = 0x06,
    ERROR = 0x07, // Error: sender emits reason string, both sides close connection
    CANCEL = 0x08, // CANCEL: receiver requests abort, sender stops and closes
};

// Wire Format
// all multi-bte fields are big-endian (network byte order)
// Header: [version:u8][type:u8][payload_len:u32 BE]
// FILE_META payload: [file_size:u64 BE][filename_len:u32 BE][filename:utf8]
// COMPLETE payload: empty
// ERROR payload: [reason:utf8] (length = payload_len)
struct Header {
    uint8_t version;
    MsgType type;
    uint32_t payload_len; // host order here, big-endian on wire
};

// Buffer
// move-only, supports both owned and pool-handle paths
// v0.1 heap: Buffer(n) - owned = make_unique<uint8_[]>(n), release = nullptr
// v0.3 mmap: Buffer(ptr, n, [n](uint8_t* p){ munmap(p, n); }
// v0.4 io_uring registered pool: Buffer(ptr, n, [&pool](uint8_t* p) { pool.release(p); })
// std::function as type-erased release callback unifies three memory ownership models
struct Buffer {
    std::unique_ptr<uint8_t[]> owned; // on-demand allocation path -> heap
    uint8_t* ptr{}; // pool handle path (io_uring)
    size_t size{};
    std::function<void(uint8_t*)> release;

    Buffer() = default;

    // heap path - owned handles cleanup
    explicit Buffer(size_t n)
        : owned(std::make_unique<uint8_t[]>(n)), size(n) {}

    // mmap / pool path - release callback handles cleanup
    Buffer(uint8_t* p, size_t n, std::function<void(uint8_t*)> rel)
        : ptr(p), size(n), release(std::move(rel)) {}

    ~Buffer() {
        if (!owned && ptr && release) { release(ptr); }
    }

    // disable copy
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    // enable move
    Buffer(Buffer&& other) noexcept
        : owned (std::move(other.owned))
        , ptr (std::exchange(other.ptr, nullptr))
        , size (std::exchange(other.size, 0))
        , release (std::exchange(other.release,nullptr)) {}

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            if (!owned && ptr && release) { release(ptr); }
            owned = std::move(other.owned);
            ptr = std::exchange(other.ptr, nullptr);
            size = std::exchange(other.size, 0);
            release = std::exchange(other.release,nullptr);
        }
        return *this;
    }

    uint8_t* get() { return owned ? owned.get() : ptr; }
    const uint8_t* get() const { return owned ? owned.get() : ptr; }
    bool valid() const { return get() != nullptr && size > 0; } // avoid: COMPLETE msg type is empty() == true
};

// Message
// move only, control plane only
// Data plane (v0.1): raw file bytes send via send_exact, bypassing Message
// v0.2+: raw file bytes send as Message, bypass removed
struct Message {
    MsgType type{}; // zero-inited - 0x00 is INVALID MsgType
    Buffer payload;

    Message() = default;
    Message(MsgType t, Buffer payload) : type(t), payload(std::move(payload)) {}

    // disable copy
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;

    // enable move
    Message(Message&&) = default;
    Message& operator=(Message&&) = default;
};

// Typed payload structs, in-memory only
struct FileMeta {
    std::string filename;
    uint64_t file_size{}; // bytes, big-endian on wire
};

// Exact I/O, inline, Transport& only
inline void send_exact(Transport& t, const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = t.send(buf + sent, len - sent);
        if (n == 0) { throw std::runtime_error("send_exact: connection closed"); }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { continue; }
            throw std::runtime_error(
                std::string("send_exact: ") + std::strerror(errno));
        }
        sent += static_cast<size_t>(n);
    }
}

inline void recv_exact(Transport& t, uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = t.recv(buf + got, len - got);
        if (n == 0) { throw std::runtime_error("recv_exact: connection closed"); }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { continue; }
            throw std::runtime_error(
                std::string("recv_exact: ") + std::strerror(errno));
        }
        got += static_cast<size_t>(n);
    }
}

// Public APIs
void send_msg(Transport& t, Message&& msg); // move only
Message recv_msg(Transport& t);

// Builders
Message make_filemeta(FileMeta meta);
Message make_complete();
Message make_error(std::string reason);

// Parsers
FileMeta parse_filemeta(const Message& msg);
std::string parse_error(const Message& msg);