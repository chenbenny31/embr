//
// Created by benny on 3/15/26.
//

#include "pull.hpp"
#include "protocol.hpp"
#include "chunk_manager.hpp"
#include "hash.hpp"
#include "../util/socket_fd.hpp"
#include "../util/constants.hpp"
#include <fcntl.h>
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
        throw std::runtime_error("run pull: expected FILE_META");
    }
    FileMeta file_meta = parse_filemeta(file_meta_msg);
    std::cout << "[pull] file=" << file_meta.filename
              << " size=" << file_meta.file_size
              << " chunks=" << file_meta.chunk_count << "\n";

    // open output file
    const std::string& out = output_path.empty() ? file_meta.filename : output_path;
    int raw_fd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
    Buffer buf(file_meta.chunk_size);

    while (!cm.all_done()) {
        // send CHUNK_REQ
        uint32_t idx = 0;
        for (uint32_t i = 0; i < file_meta.chunk_count; ++i) {
            if (!cm.is_done(i)) {
                idx = i;
                break;
            }
        }
        send_msg(transport, make_chunk_req(ChunkReq{idx}));

        // recv CHUNK_HDR header
        Message chunk_hdr_msg = recv_msg(transport);
        if (chunk_hdr_msg.type != MsgType::CHUNK_HDR) {
            throw std::runtime_error("run_pull: expected CHUNK_HDR");
        }
        ChunkHdr chunk_hdr = parse_chunk_hdr(chunk_hdr_msg);

        if (chunk_hdr.chunk_index != idx) {
            throw std::runtime_error("run_pull: received chunk index " +
                std::to_string(chunk_hdr.chunk_index) +
                " expected " + std::to_string(idx));
        }

        // compute this chunk's length
        uint64_t offset = static_cast<uint64_t>(idx) * file_meta.chunk_size;
        uint64_t remain = file_meta.file_size - offset;
        auto chunk_len =  static_cast<size_t>(
            std::min(static_cast<uint64_t>(file_meta.chunk_size), remain));

        // recv raw bytes - data plane
        recv_exact(transport, buf.get(), chunk_len);

        // verify chunk hash
        auto computed_hash = sha256_buf(buf.get(), chunk_len);
        if (computed_hash != chunk_hdr.chunk_hash) {
            throw std::runtime_error("run_pull: hash mismatch at chunk " +
                std::to_string(idx));
        }

        // write to correct offset
        ssize_t written = ::pwrite(fd.get(), buf.get(), chunk_len,
            static_cast<off_t>(offset));
        if (written != static_cast<ssize_t>(chunk_len)) {
            throw std::runtime_error("run_pull: failed to write chunk " +
                std::to_string(idx) + " - " + std::strerror(errno));
        }

        cm.mark_done(idx);
        std::cout << "[pull] chunk " << idx + 1 << "/" << file_meta.chunk_count
                  << "\r" << std::flush;
    }
    std::cout << "\n[pull] all chunks received\n";

    // send complete
    send_msg(transport, make_complete());
    std::cout << "[pull] transfer complete - saved to " << out << "\n";
}