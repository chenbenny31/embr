//
// Created by benny on 3/14/26.
//

#include "push.hpp"
#include "protocol.hpp"
#include "chunk_manager.hpp"
#include "hash.hpp"
#include "../util/constants.hpp"
#include "../util/socket_fd.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <algorithm>

void run_push(Transport& transport, const std::string& filepath) {
    // open file
    int raw_fd = ::open(filepath.c_str(), O_RDONLY);
    if (raw_fd < 0) {
        throw std::runtime_error("run_push: failed to open " + filepath +
                                 " - " + std::strerror(errno));
    }
    SocketFd fd(raw_fd);

    uint64_t file_size = std::filesystem::file_size(filepath);
    if (file_size == 0) {
        throw std::runtime_error("run_push: file is empty: " + filepath);
    }

    std::string filename = std::filesystem::path(filepath).filename().string();
    auto chunk_count = static_cast<uint32_t>(
        (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE);

    // recv handshake
    Message hs = recv_msg(transport);
    if (hs.type != MsgType::HANDSHAKE) {
        throw std::runtime_error("run_push: expected HANDSHAKE");
    }
    auto handshake = parse_handshake(hs);

    std::cout << "[push] peer connected, token='" // v0.2: token is empty
              << handshake.token <<"'\n";

    // send FILE_META
    send_msg(transport, make_filemeta(FileMeta{
        .filename = filename,
        .file_size = file_size,
        .chunk_size = static_cast<uint32_t>(CHUNK_SIZE),
        .chunk_count = chunk_count,
    }));
    std::cout << "[push] sent FILE_META - file=" << filename
              << " size=" << file_size
              << " chunks=" << chunk_count << "\n";

    // serve chunk requests
    ChunkManager cm(chunk_count);
    Buffer buf(CHUNK_SIZE);

    while (!cm.all_done()) {
        Message req = recv_msg(transport);
        if (req.type != MsgType::CHUNK_REQ) {
            throw std::runtime_error("run_push: expected CHUNK_REQ");
        }
        auto chunk_req = parse_chunk_req(req);
        uint32_t idx = chunk_req.chunk_index;
        uint64_t offset = static_cast<uint64_t>(idx) * CHUNK_SIZE;
        uint64_t remain = file_size - offset;
        auto chunk_len = static_cast<size_t>(
            std::min(static_cast<uint64_t>(CHUNK_SIZE), remain));

        // pread: atomic, no modify file offset
        ssize_t bytes_read = ::pread(fd.get(), buf.get(), chunk_len,
            static_cast<off_t>(offset));
        if (bytes_read != static_cast<ssize_t>(chunk_len)) {
            throw std::runtime_error("run_push: pread failed at chunk: " +
                                     std::to_string(idx) + " - " +
                                     std::strerror(errno));
        }

        auto chunk_hash = sha256_buf(buf.get(), chunk_len);

        // send CHUNK_DATA header (Message) + raw bytes (data plane)
        send_msg(transport, make_chunk_hdr({
            .chunk_index = idx,
            .chunk_hash = chunk_hash,
        }));
        send_exact(transport, buf.get(), chunk_len);

        cm.mark_done(idx);
        std::cout << "[push] chunk " << idx + 1 << "/" << chunk_count << "\r"
                  << std::flush;
    }
    std::cout << "\n[push] all chunks sent\n";

    // recv COMPLETE
    Message done = recv_msg(transport);
    if (done.type != MsgType::COMPLETE) {
        throw std::runtime_error("run_push: expected COMPLETE");
    }
    std::cout << "[push] transfer complete\n";
}