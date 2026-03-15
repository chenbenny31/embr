//
// Created by benny on 3/13/26.
//

#include "protocol.hpp"
#include <arpa/inet.h>

// Internal scope
namespace {

    void put_u32(Buffer& buf, size_t& pos, uint32_t val) {
        uint32_t net = htonl(val);
        std::memcpy(buf.get() + pos, &net, sizeof(net));
        pos += sizeof(net);
    }

    void put_u64(Buffer& buf, size_t& pos, uint64_t val) {
        // big-endian: high word first (MSB first)
        put_u32(buf, pos, static_cast<uint32_t>(val >> 32));
        put_u32(buf, pos, static_cast<uint32_t>(val));
    }

    void put_str(Buffer& buf, size_t& pos, const std::string& str) {
        put_u32(buf, pos, static_cast<uint32_t>(str.size())); // length prefix
        std::memcpy(buf.get() + pos, str.data(), str.size()); // raw UTF-8, no null terminator
        pos += str.size();
    }

    struct Reader {
        const uint8_t* data;
        size_t pos;
        size_t len;

        // Bound checks before read, throws if truncated/malformed payload
        void ensure(size_t n) const {
            if (pos + n > len) { throw std::runtime_error("protocol: truncated payload"); }
        }

        uint32_t get_u32() {
            ensure(sizeof(uint32_t)); // de-serialization primitive
            uint32_t net{};
            std::memcpy(&net, data + pos, sizeof(uint32_t));
            pos += sizeof(uint32_t);
            return ntohl(net);
        }

        uint64_t get_u64() {
            uint64_t hi = get_u32();
            uint64_t lo = get_u32();
            return (hi << 32) | lo;
        }

        std::string get_str() {
            uint32_t str_len = get_u32();
            ensure(str_len);
            std::string str(reinterpret_cast<const char*>(data + pos), str_len);
            pos += str_len;
            return str;
        }
    };

}

void send_msg(Transport& t, Message&& msg) {
    uint8_t hdr[HEADER_SIZE];
    hdr[0] = PROTOCOL_VERSION;
    hdr[1] = static_cast<uint8_t>(msg.type);
    uint32_t net_len = htonl(static_cast<uint32_t>(msg.payload.size));
    std::memcpy(hdr + 2, &net_len, sizeof(net_len));

    send_exact(t, hdr, HEADER_SIZE);
    if (msg.payload.size > 0) { send_exact(t, msg.payload.get(), msg.payload.size); }
}

Message recv_msg(Transport& t) {
    uint8_t hdr[HEADER_SIZE];
    recv_exact(t, hdr, HEADER_SIZE);

    if (hdr[0] != PROTOCOL_VERSION) {
        throw std::runtime_error("recv_msg: unsupported protocol version " + std::to_string(hdr[0]));
    }

    // validate MsgType range
    if (hdr[1] < static_cast<uint8_t>(MsgType::HANDSHAKE) || hdr[1] > static_cast<uint8_t>(MsgType::CANCEL)) {
        throw std::runtime_error("recv_msg: unknown message type " + std::to_string(hdr[1]));
    }

    Message msg;
    msg.type = static_cast<MsgType>(hdr[1]);

    uint32_t net_len{};
    std::memcpy(&net_len, hdr + 2, sizeof(net_len));
    uint32_t payload_len = ntohl(net_len);

    // guard against malformed headers
    if (payload_len > MAX_PAYLOAD_SIZE) {
        throw std::runtime_error("recv_msg: payload_len exceeds MAX_PAYLOAD_SIZE (" +
                                 std::to_string(payload_len) + "bytes)");
    }

    if (payload_len > 0) {
        msg.payload = Buffer(payload_len);
        recv_exact(t, msg.payload.get(), payload_len);
    }

    return msg;
}

Message make_filemeta(FileMeta meta) {
    // wire layout: : [file_size:u64 BE][filename_len:u32 BE][filename:utf8]
    size_t payload_size = FILE_SIZE_BYTES + LEN_PREFIX_BYTES + meta.filename.size();
    Buffer buf(payload_size);
    size_t pos = 0;
    put_u64(buf, pos, meta.file_size);
    put_str(buf, pos, meta.filename);
    return Message{MsgType::FILE_META, std::move(buf)};
}

Message make_complete() {
    return Message{MsgType::COMPLETE, Buffer{}};
}

Message make_error(std::string reason) {
    // ERROR payload: raw UFT-8 bytes, length implied by payload_len in header, no length prefix
    Buffer buf(reason.size());
    std::memcpy(buf.get(), reason.data(), reason.size());
    return Message{MsgType::ERROR, std::move(buf)};
}

FileMeta parse_filemeta(const Message& msg) {
    if (msg.type != MsgType::FILE_META) { throw std::runtime_error("parse_filemeta: wrong type"); }
    Reader reader{msg.payload.get(), 0, msg.payload.size};
    FileMeta meta;
    meta.file_size = reader.get_u64();
    meta.filename = reader.get_str();
    return meta;
}

std::string parse_error(const Message& msg) {
    if (msg.type != MsgType::ERROR) { throw std::runtime_error("parse_error: wrong type"); }
    return std::string(reinterpret_cast<const char*>(msg.payload.get()), msg.payload.size);
}
