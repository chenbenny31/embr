//
// Created by benny on 3/23/26.
//

#include "hash.hpp"
#include "util/constants.hpp"
#include "util/exact_io.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// .embr.hash format:
//   [file_size:u64][mtime_ns:u64][chunk_size:u32][chunk_count:u32]
//   [chunk_hashes: chunk_count * HASH_SIZE bytes]

namespace {

// RAII wrapper for EVP_MD_CTX, guarantees free on throw
struct EvpCtx {
    EVP_MD_CTX* ctx;
    EvpCtx() : ctx(EVP_MD_CTX_new()) {
        if (!ctx) { throw std::runtime_error("EVP_MD_CTX_new failed"); }
    }
    ~EvpCtx() { EVP_MD_CTX_free(ctx); }
    EvpCtx(const EvpCtx&) = delete;
    EvpCtx& operator=(const EvpCtx&) = delete;
};

}

std::array<uint8_t, HASH_SIZE> sha256_buf(const uint8_t* data, size_t len) {
    EvpCtx evp;
    if (!EVP_DigestInit_ex(evp.ctx, EVP_sha256(), nullptr)) {
        throw std::runtime_error("sha256_buf: EVP_DigestInit_ex() failed");
    }
    if (!EVP_DigestUpdate(evp.ctx, data, len)) {
        throw std::runtime_error("sha256_buf: EVP_DigestUpdate() failed");
    }
    std::array<uint8_t, HASH_SIZE> digest{};
    unsigned int digest_len = 0;
    if (!EVP_DigestFinal_ex(evp.ctx, digest.data(), &digest_len)) {
        throw std::runtime_error("sha256_buf: EVP_DigestFinal_ex() failed");
    }
    return digest;
}

std::string hash_cache_path_for(const std::string& file_path) {
    if (file_path.empty()) {
        throw std::runtime_error("hash_cache_path_for: file_path is empty");
    }
    const size_t slash_pos = file_path.rfind('/');
    if (slash_pos == std::string::npos) {
        return '.' + file_path + ".embr.hash";
    }
    return file_path.substr(0, slash_pos + 1) + '.' +
           file_path.substr(slash_pos + 1) + ".embr.hash";
}

bool hash_cache_load(const std::string& file_path,
                     uint64_t file_size,
                     uint64_t mtime_ns,
                     uint32_t chunk_count,
                     std::vector<std::array<uint8_t, HASH_SIZE>>& chunk_hashes) {
    const std::string cache_path = hash_cache_path_for(file_path);
    int fd = ::open(cache_path.c_str(), O_RDONLY);
    if (fd < 0) { return false; }

    try {
        uint64_t stored_size{};
        uint64_t stored_mtime{};
        uint32_t stored_chunk_size{};
        uint32_t stored_count{};

        read_exact(fd, reinterpret_cast<uint8_t*>(&stored_size), sizeof(stored_size));
        read_exact(fd, reinterpret_cast<uint8_t*>(&stored_mtime), sizeof(stored_mtime));
        read_exact(fd, reinterpret_cast<uint8_t*>(&stored_chunk_size), sizeof(stored_chunk_size));
        read_exact(fd, reinterpret_cast<uint8_t*>(&stored_count), sizeof(stored_count));

        if (stored_size != file_size ||
            stored_mtime != mtime_ns ||
            stored_count != chunk_count) {
            ::close(fd);
            return false;
        }

        chunk_hashes.resize(chunk_count);
        for (auto& hash : chunk_hashes) {
            read_exact(fd, hash.data(), HASH_SIZE);
        }
        ::close(fd);
        return true;

    } catch (const std::runtime_error& e) {
        ::close(fd);
        return false; // corrupted cache - fall back to re-compute
    }
}

void hash_cache_save(const std::string& file_path,
                     uint64_t file_size,
                     uint64_t mtime_ns,
                     uint32_t chunk_size,
                     uint32_t chunk_count,
                     std::vector<std::array<uint8_t, HASH_SIZE>>& chunk_hashes) {
    const std::string cache_path = hash_cache_path_for(file_path);
    int fd = ::open(cache_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error("hash_cache_save: failed to open: " + cache_path);
    }

    try {
        write_exact(fd, reinterpret_cast<uint8_t*>(&file_size), sizeof(file_size));
        write_exact(fd, reinterpret_cast<uint8_t*>(&mtime_ns), sizeof(mtime_ns));
        write_exact(fd, reinterpret_cast<uint8_t*>(&chunk_size), sizeof(chunk_size));
        write_exact(fd, reinterpret_cast<uint8_t*>(&chunk_count), sizeof(chunk_count));

        for (const auto& hash : chunk_hashes) {
            write_exact(fd, hash.data(), HASH_SIZE);
        }
        ::close(fd);

    } catch (...) {
        ::close(fd);
        throw;
    }
}