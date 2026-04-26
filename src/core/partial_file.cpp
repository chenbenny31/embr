//
// Created by benny on 4/19/26.
//

#include "partial_file.hpp"
#include "chunk_manager.hpp"
#include "util/constants.hpp"
#include "util/exact_io.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// .embr.partial format: (local cache only)
//   [file_hash: 32 bytes]
//   [bitmap: ceil(chunk_count / 8) bytes, LSB = chunk 0]

std::string PartialFile::path_for(const std::string& output_path) {
    const size_t slash_pos = output_path.rfind('/');
    if (slash_pos == std::string::npos) {
        return '.' + output_path + ".embr.partial";
    }
    return output_path.substr(0, slash_pos + 1) + '.'
           + output_path.substr(slash_pos + 1) + ".embr.partial";
}

std::optional<ChunkManager> PartialFile::load(const std::string& output_path,
        const std::array<uint8_t, HASH_SIZE>& file_hash, uint32_t chunk_count) {
    const std::string partial_path = path_for(output_path);
    int fd = ::open(partial_path.c_str(), O_RDONLY);
    if (fd < 0) { return std::nullopt; } // no partial file (fresh)

    try {
        std::array<uint8_t, HASH_SIZE> stored_hash{};
        read_exact(fd, stored_hash.data(), sizeof(stored_hash));
        if (std::memcmp(stored_hash.data(), file_hash.data(), sizeof(stored_hash)) != 0) {
            ::close(fd);
            return std::nullopt; // different file (treat as fresh)
        }

        struct stat file_stat{};
        if (::stat(output_path.c_str(), &file_stat) < 0 ||
            static_cast<uint64_t>(file_stat.st_size) != static_cast<uint64_t>(chunk_count) * CHUNK_SIZE) {
            ::close(fd);
            return std::nullopt;
        }

        const size_t bitmap_bytes = (chunk_count + 8 - 1) / 8; // ceil
        std::vector<uint8_t> raw(bitmap_bytes);
        read_exact(fd, raw.data(), bitmap_bytes);
        ::close(fd);

        ChunkManager chunk_manager(chunk_count);
        for (uint32_t i = 0; i < chunk_count; ++i) {
            if (raw[i / 8] & (1u << (i % 8))) {
                chunk_manager.mark_done(i);
            }
        }
        return chunk_manager;

    } catch (const std::runtime_error& e) {
        ::close(fd);
        return std::nullopt; // corrupted partial -- treat as fresh
    }
}

void PartialFile::save(const std::string& output_path,
        const std::array<uint8_t, HASH_SIZE>& file_hash,
        const ChunkManager& chunk_manager) {
    const std::string partial_path = path_for(output_path);
    int fd = ::open(partial_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error("partial_file::save: failed to open for write: " + partial_path);
    }

    try {
        write_exact(fd, file_hash.data(), HASH_SIZE);

        const uint32_t chunk_count = chunk_manager.chunk_count();
        const size_t bitmap_bytes = (chunk_count + 8 - 1) / 8; // ceil
        std::vector<uint8_t> raw(bitmap_bytes, 0);
        const std::vector<bool>& bitmap = chunk_manager.bitmap();
        for (uint32_t i = 0; i < chunk_count; ++i) {
            if (bitmap[i]) { raw[i / 8] |= (1u << (i % 8)); }
        }
        write_exact(fd, raw.data(), bitmap_bytes);
        ::close(fd);
    } catch (...) {
        ::close(fd);
        throw;
    }
}

void PartialFile::remove(const std::string& output_path) {
    ::unlink(path_for(output_path).c_str());
}