//
// Created by benny on 3/14/26.
//

#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <sys/mman.h>
#include "push.hpp"
#include "protocol.hpp"
#include "chunk_manager.hpp"
#include "hash.hpp"
#include "../util/constants.hpp"
#include "../util/socket_fd.hpp"

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

    std::string file_name = std::filesystem::path(filepath).filename().string();
    auto chunk_count = static_cast<uint32_t>(
        (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE);

    // pre-hash: mmap entire file, sha256 per chunk
    std::cout << "[push] hashing " << file_name
              << " (" << chunk_count << " chunks)...\n";

    void* file_map = ::mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd.get(), 0);

    if (file_map == MAP_FAILED) {
        throw std::runtime_error(
            std::string("run_push: mmap failed: ") + std::strerror(errno));
    }

    FileMeta file_meta{
        .file_name = file_name,
        .file_size = file_size,
        .chunk_size = static_cast<uint32_t>(CHUNK_SIZE),
        .chunk_count = chunk_count,
    };
    file_meta.chunk_hashes.resize(chunk_count);

    for (uint32_t i = 0; i < chunk_count; ++i) {
        uint64_t offset = static_cast<uint64_t>(i) * CHUNK_SIZE;
        uint64_t remain = file_size - offset;
        size_t chunk_len = static_cast<size_t>(
            std::min(static_cast<uint64_t>(CHUNK_SIZE), remain));
        file_meta.chunk_hashes[i] = sha256_buf(static_cast<uint8_t*>(file_map) + offset, chunk_len);
        std::cout << "[push] hashing " << i + 1 << "/" << chunk_count << "\r" << std::flush;
    }
    ::munmap(file_map, file_size);
    std::cout << "\n[push] hashing complete\n";

    // recv handshake
    Message hs = recv_msg(transport);
    if (hs.type != MsgType::HANDSHAKE) {
        throw std::runtime_error("run_push: expected HANDSHAKE");
    }
    auto handshake = parse_handshake(hs);

    std::cout << "[push] peer connected, token='" // v0.2: token is empty
              << handshake.token <<"'\n";

    // send FILE_META
    send_msg(transport, make_filemeta(std::move(file_meta)));
    std::cout << "[push] sent FILE_META - file=" << file_name
              << " size=" << file_size
              << " chunks=" << chunk_count << "\n";

    // stream chunks
    ChunkManager cm(chunk_count);

    for (uint32_t i = 0; i < chunk_count; ++i) {
        uint64_t offset = static_cast<uint64_t>(i) * CHUNK_SIZE;
        uint64_t remain = file_size - offset;
        size_t chunk_len = static_cast<size_t>(
            std::min(static_cast<uint64_t>(CHUNK_SIZE), remain));

        // send CHUNK_HDR
        send_msg(transport, make_chunk_hdr(ChunkHdr{.chunk_index = i}));

        // send_file: sendfile() system call, 0 copy on TCP path
        transport.send_file(fd.get(), offset, chunk_len);

        cm.mark_done(i);
        std::cout << "[push] chunk " << i + 1 << "/" << chunk_count << "\r" << std::flush;
    }
    std::cout << "\n[push] all chunks sent\n";

    // recv COMPLETE
    Message done = recv_msg(transport);
    if (done.type != MsgType::COMPLETE) {
        throw std::runtime_error("run_push: expected COMPLETE");
    }
    std::cout << "[push] transfer complete\n";
};