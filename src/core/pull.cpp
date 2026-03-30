//
// Created by benny on 3/15/26.
//

#include "pull.hpp"

#include "protocol.hpp"
#include "chunk_manager.hpp"
#include "hash.hpp"
#include "../util/socket_fd.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <cstring>

void run_pull(Transport& transport, const std::string& output_path) {
    // send HANDSHAKE
    send_msg(transport, make_handshake(HandshakePayload{""}));

    // recv FILE_META
    Message file_meta_msg = recv_msg(transport);
    if (file_meta_msg.type != MsgType::FILE_META) {
        throw std::runtime_error("run_pull: expected FILE_META");
    }
    FileMeta file_meta = parse_filemeta(file_meta_msg);
    std::cout << "[pull] file=" << file_meta.file_name
              << " size=" << file_meta.file_size
              << " chunks=" << file_meta.chunk_count << "\n";

    // open output file
    const std::string& out = output_path.empty() ? file_meta.file_name : output_path;
    int raw_fd = ::open(out.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (raw_fd < 0) {
        throw std::runtime_error("run_pull: failed to open file " + out +
                                 " - " + std::strerror(errno));
    }
    SocketFd fd(raw_fd);

    // pre-allocate file size: enables pwrite at arbitrary offsets
    if (::ftruncate(fd.get(), static_cast<off_t>(file_meta.file_size)) < 0) {
        throw std::runtime_error("run_pull: failed to truncate file - " +
                                 std::string(std::strerror(errno)));
    }

    // recv chunks
    ChunkManager cm(file_meta.chunk_count);

    for (uint32_t i = 0; i < file_meta.chunk_count; ++i) {
        // recv CHUNK_HDR
        Message chunk_hdr_msg = recv_msg(transport);
        if (chunk_hdr_msg.type != MsgType::CHUNK_HDR) {
            throw std::runtime_error("run_pull: expected CHUNK_HDR");
        }

        ChunkHdr chunk_hdr = parse_chunk_hdr(chunk_hdr_msg);
        if (chunk_hdr.chunk_index != i) {
            throw std::runtime_error("run_pull: unexpected chunk_index " +
                std::to_string(chunk_hdr.chunk_index) + " expected " + std::to_string(i));
        }

        uint64_t offset = static_cast<uint64_t>(i) * file_meta.chunk_size;
        uint64_t remain = file_meta.file_size - offset;
        size_t chunk_len = static_cast<size_t>(
            std::min(static_cast<uint64_t>(file_meta.chunk_size), remain));

        // recv_file: mmap(MAP_SHARED) + recv - 1 copy, socket buf -> page cache
        transport.recv_file(fd.get(), offset, chunk_len);

        // verify hash - mmap same region (page cache on hot path)
        void* mapped = ::mmap(nullptr, chunk_len, PROT_READ, MAP_SHARED,
            fd.get(), static_cast<off_t>(offset));
        if (mapped == MAP_FAILED) {
            throw std::runtime_error(
                std::string("run_pull: mmap for verify failed: ") + std::strerror(errno));
        }
        auto computed = sha256_buf(static_cast<const uint8_t*>(mapped), chunk_len);
        ::munmap(mapped, chunk_len);
        if (computed != file_meta.chunk_hashes[i]) {
            throw std::runtime_error("run_pull: hash mismatch at chunk " + std::to_string(i));
        }

        cm.mark_done(i);
        std::cout << "[pull] chunk " << i + 1 << "/" << file_meta.chunk_count << "\r" << std::flush;
    }
    std::cout << "\n[pull] all chunks received\n";

    // send COMPLETE
    send_msg(transport, make_complete());
    std::cout << "[pull] transfer complete - saved to " << out << "\n";
}