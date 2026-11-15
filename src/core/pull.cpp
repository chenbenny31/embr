//
// Created by benny on 3/15/26.
//

#include "chunk_manager.hpp"
#include "hash.hpp"
#include "protocol.hpp"
#include "partial_file.hpp"
#include "pull.hpp"
#include "transport/udp_transport.hpp"
#include "util/socket_fd.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <optional>

namespace {

// true when the bytes on disk for one chunk hash to the value the sender announced
bool chunk_on_disk_matches(int fd, uint32_t chunk_index, uint64_t offset, uint64_t len,
                           const std::array<uint8_t, HASH_SIZE>& expected) {
    void* mapped = ::mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(offset));
    if (mapped == MAP_FAILED) {
        throw std::runtime_error("run_pull: mmap for verify failed at chunk " +
                                  std::to_string(chunk_index) +
                                  " - " + std::strerror(errno));
    }
    const auto computed = sha256_buf(static_cast<const uint8_t*>(mapped), len);
    ::munmap(mapped, static_cast<size_t>(len));
    return computed == expected;
}

// offset and length of chunk i; only the last chunk is short
std::pair<uint64_t, uint64_t> chunk_span(const FileMeta& meta, uint32_t i) {
    const uint64_t offset = static_cast<uint64_t>(i) * CHUNK_SIZE;
    return {offset, std::min(static_cast<uint64_t>(CHUNK_SIZE), meta.file_size - offset)};
}

// true when out already holds the whole file: right size and every chunk hash; read-only
bool already_complete(const std::string& out, const FileMeta& meta) {
    const int raw_fd = ::open(out.c_str(), O_RDONLY);
    if (raw_fd < 0) { return false; }
    SocketFd fd{raw_fd};
    struct stat st{};
    if (::fstat(fd.get(), &st) < 0 || static_cast<uint64_t>(st.st_size) != meta.file_size) {
        return false;
    }
    for (uint32_t i = 0; i < meta.chunk_count; ++i) {
        const auto [offset, len] = chunk_span(meta, i);
        if (!chunk_on_disk_matches(fd.get(), i, offset, len, meta.chunk_hashes[i])) { return false; }
    }
    return true;
}

// create the output for a fresh pull: a name the sender chose never replaces an existing file
int open_fresh(const std::string& out, bool sender_named) {
    const int flags = O_RDWR | O_CREAT | (sender_named ? O_EXCL : O_TRUNC);
    const int fd = ::open(out.c_str(), flags, 0664);
    if (fd < 0 && errno == EEXIST) {
        throw std::runtime_error("run_pull: " + out + " already exists and is a different file"
                                 " - pass --out PATH to choose where to write");
    }
    if (fd < 0) {
        throw std::runtime_error("run_pull: failed to open file " + out +
                                 " - " + std::strerror(errno));
    }
    return fd;
}

}

void run_pull(Transport& t, const std::string& output_path) {
    // send HANDSHAKE
    send_msg(t, make_handshake(HandshakePayload{""}));

    // recv FILE_META
    Message meta_msg = recv_msg(t);
    if (meta_msg.type != MsgType::FILE_META) {
        throw std::runtime_error("run_pull: expected FILE_META");
    }
    FileMeta file_meta = parse_filemeta(meta_msg);
    std::cout << "[pull] file=" << file_meta.file_name
              << " size=" << file_meta.file_size
              << " chunks=" << file_meta.chunk_count << "\n";

    // open output file
    const std::string& out = output_path.empty() ? file_meta.file_name : output_path;

    // three ways to start, decided here:
    //   resume   - out is a partial download of this file: matching .embr.partial beside it
    //   complete - out already holds the whole file: nothing to transfer
    //   fresh    - anything else: create out and fetch every chunk
    std::optional<ChunkManager> cm_opt = PartialFile::load(
        out, file_meta.chunk_hashes[0], file_meta.chunk_count, file_meta.file_size);
    const int resume_fd = cm_opt ? ::open(out.c_str(), O_RDWR) : -1;
    const bool resuming = resume_fd >= 0;

    if (!resuming && already_complete(out, file_meta)) {
        send_msg(t, make_complete()); // ends the sender's request loop with zero requests
        std::cout << "[pull] already complete - " << out
                  << " matches the sender's file, nothing to transfer\n";
        return;
    }

    SocketFd fd{resuming ? resume_fd : open_fresh(out, output_path.empty())};
    ChunkManager cm = resuming ? std::move(*cm_opt) : ChunkManager(file_meta.chunk_count);

    if (resuming) {
        // the bitmap is a hint: a chunk counts as received only if its bytes on disk hash right
        uint32_t kept = 0;
        for (uint32_t i = 0; i < file_meta.chunk_count; ++i) {
            if (cm.needs_chunk(i)) { continue; }
            const auto [offset, len] = chunk_span(file_meta, i);
            if (chunk_on_disk_matches(fd.get(), i, offset, len, file_meta.chunk_hashes[i])) {
                ++kept;
            } else {
                cm.mark_todo(i);
            }
        }
        std::cout << "[pull] resuming partial download - " << kept << " chunks verified on disk\n";
    } else {
        // fresh: size the file now so every chunk has a place to land
        if (::ftruncate(fd.get(), static_cast<off_t>(file_meta.file_size)) < 0) {
            throw std::runtime_error("run_pull: failed to truncate file - " +
                                     std::string(std::strerror(errno)));
        }
    }

    // send CHUNK_REQ for needed chunks, then COMPLETE
    const std::vector<uint32_t> needed = cm.needed_chunks();
    std::cout << "[pull] requesting " << needed.size()
              << "/" << file_meta.chunk_count << " chunks\n";
    for (uint32_t chunk_index : needed) {
        send_msg(t, make_chunk_req(ChunkReq{chunk_index}));
    }
    send_msg(t, make_complete());

    for (uint32_t chunk_index : needed) {
        // recv CHUNK_HDR
        Message hdr_msg = recv_msg(t);
        if (hdr_msg.type != MsgType::CHUNK_HDR) {
            throw std::runtime_error("run_pull: expected CHUNK_HDR for chunk "  +
                                     std::to_string(chunk_index));
        }

        ChunkHdr hdr = parse_chunk_hdr(hdr_msg);
        if (hdr.chunk_index != chunk_index) {
            throw std::runtime_error("run_pull: chunk index mismatch - expected " +
                                     std::to_string(chunk_index) +
                                     " got " + std::to_string(hdr.chunk_index));
        }

        uint64_t offset = static_cast<uint64_t>(chunk_index) * CHUNK_SIZE;
        uint64_t chunk_len = std::min(static_cast<uint64_t>(CHUNK_SIZE),
                                      file_meta.file_size - offset);

        // recv chunk data into output file
        t.recv_file(fd.get(), offset, chunk_len);

        // verify chunk hash
        if (chunk_on_disk_matches(fd.get(), chunk_index, offset, chunk_len,
                                  file_meta.chunk_hashes[chunk_index])) {
            cm.mark_done(chunk_index);
        } else {
            cm.mark_todo(chunk_index);
            std::cerr << "\n[pull] hash mismatch at chunk " << chunk_index << "\n";
        }
        PartialFile::save(out, file_meta.chunk_hashes[0], cm);

        std::cout << "[pull] received " << chunk_index + 1
                  << "/" << file_meta.chunk_count << "\r" << std::flush;
    }
    std::cout << "\n";

    if (cm.all_done()) {
        PartialFile::remove(out);
        std::cout << "[pull] transfer complete - saved to " << out << "\n";
    } else {
        uint32_t todo_chunks = static_cast<uint32_t>(cm.needed_chunks().size());
        std::cout << "[pull] transfer incomplete - "
                  << todo_chunks << " chunks failed verify, resume to retry\n";
    }
}