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
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <optional>

namespace {

// the chunks among these whose bytes on disk do not hash to what the sender announced
// hashed in rounds of one set of lanes per hash thread; with first_only the rounds after one that
// found a mismatch are skipped, so a different file of the same size is not read to its end
std::vector<uint32_t> mismatched_on_disk(int fd, const FileMeta& meta,
                                         std::span<const uint32_t> chunk_indexes, bool first_only) {
    const size_t round = static_cast<size_t>(sha256_lanes()) *
                         std::max(1u, std::thread::hardware_concurrency());
    std::vector<uint32_t> mismatched;
    for (size_t first = 0; first < chunk_indexes.size(); first += round) {
        const auto indexes = chunk_indexes.subspan(first, std::min(round, chunk_indexes.size() - first));
        const auto digests = hash_chunks_parallel(fd, meta.file_size, indexes);
        for (size_t k = 0; k < indexes.size(); ++k) {
            if (digests[k] != meta.chunk_hashes[indexes[k]]) { mismatched.push_back(indexes[k]); }
        }
        if (first_only && !mismatched.empty()) { break; }
    }
    return mismatched;
}

// offset and length of chunk i; only the last chunk is short
std::pair<uint64_t, uint64_t> chunk_span(const FileMeta& meta, uint32_t i) {
    const uint64_t offset = static_cast<uint64_t>(i) * CHUNK_SIZE;
    return {offset, std::min(static_cast<uint64_t>(CHUNK_SIZE), meta.file_size - offset)};
}

// CPUs this process may run on: taskset and container cpusets count here, hardware_concurrency() ignores them
unsigned usable_cpus() {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0) { return 1; }
    return static_cast<unsigned>(CPU_COUNT(&allowed));
}

// one hash thread verifies about 1 GiB/s without SHA extensions and 2.7 with them: more than four
// are only wanted beyond 30 Gbps
constexpr unsigned MAX_VERIFY_THREADS = 4;

// a full set of lanes hashes several times faster than chunk by chunk, so the verifier waits this
// long for one; it is also how far verification may trail the data on a slow link
constexpr auto VERIFY_BATCH_WAIT = std::chrono::milliseconds(200);

// verifier threads: one per CPU besides the receiving thread's, 0 verifies inline.
// EMBR_VERIFY=inline|thread forces none or at least one, EMBR_VERIFY_THREADS=N the count
unsigned verify_threads() {
    const char* mode = std::getenv("EMBR_VERIFY");
    if (mode != nullptr && std::string_view{mode} == "inline") { return 0; }
    if (const char* count = std::getenv("EMBR_VERIFY_THREADS")) {
        return static_cast<unsigned>(std::clamp(std::atoi(count), 1, 64));
    }
    // hashing side by side, one thread does several GiB/s: more would only split its batches
    const unsigned wanted = sha256_lanes() > 1 ? 1 : MAX_VERIFY_THREADS;
    const unsigned spare_cpus = std::min(usable_cpus() - 1, wanted);
    if (mode != nullptr && std::string_view{mode} == "thread") { return std::max(spare_cpus, 1u); }
    return spare_cpus;
}

// Hashes received chunks off the receiving thread: without SHA extensions one chunk's hash takes
// about as long as receiving it, and nothing is read from the socket while it runs inline.
// A thread takes up to a set of lanes at a time; several threads share the queue, so results come
// back in any order.
class ChunkVerifier {
public:
    struct Result {
        uint32_t chunk_index;
        bool matches;
    };

    ChunkVerifier(int fd, const FileMeta& meta, unsigned threads) : fd_(fd), meta_(meta) {
        for (unsigned i = 0; i < threads; ++i) { workers_.emplace_back([this] { run(); }); }
    }
    ~ChunkVerifier() { stop(); }
    ChunkVerifier(const ChunkVerifier&) = delete;
    ChunkVerifier& operator=(const ChunkVerifier&) = delete;

    // the chunk is complete on disk. With no thread it is hashed here, once a set of lanes has
    // collected or the oldest waiting chunk has waited long enough
    void submit(uint32_t chunk_index) {
        if (workers_.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (pending_.empty()) { oldest_pending_ = now; }
            pending_.push_back(chunk_index);
            if (pending_.size() >= sha256_lanes() || now - oldest_pending_ >= VERIFY_BATCH_WAIT) {
                verify_pending_here();
            }
            return;
        }
        {
            std::lock_guard lock(mutex_);
            pending_.push_back(chunk_index);
        }
        wake_.notify_one();
    }

    // results ready so far; never waits
    std::vector<Result> take_finished() {
        std::lock_guard lock(mutex_);
        return std::exchange(finished_, {});
    }

    // waits until every submitted chunk is verified
    std::vector<Result> finish() {
        stop();
        if (workers_.empty()) { verify_pending_here(); }
        return std::exchange(finished_, {});
    }

    // an error on the verifier thread (a failed mmap) surfaces on the receiving thread here
    void rethrow_if_failed() {
        std::lock_guard lock(mutex_);
        if (error_) { std::rethrow_exception(error_); }
    }

private:
    // no thread: what has collected is hashed on the calling thread
    void verify_pending_here() {
        if (pending_.empty()) { return; }
        const std::vector<uint32_t> batch(pending_.begin(), pending_.end());
        pending_.clear();
        const std::vector<Result> results = verify(batch);
        finished_.insert(finished_.end(), results.begin(), results.end());
    }

    // hashes of the chunks as they are on disk, against what the sender announced
    std::vector<Result> verify(std::span<const uint32_t> chunk_indexes) const {
        std::vector<HashInput> inputs;
        inputs.reserve(chunk_indexes.size());
        struct Unmap {
            std::vector<HashInput>& mapped;
            ~Unmap() { for (const HashInput& chunk : mapped) { ::munmap(const_cast<uint8_t*>(chunk.data), chunk.len); } }
        } unmap{inputs};
        for (const uint32_t chunk_index : chunk_indexes) {
            const auto [offset, len] = chunk_span(meta_, chunk_index);
            void* mapped = ::mmap(nullptr, len, PROT_READ, MAP_SHARED, fd_, static_cast<off_t>(offset));
            if (mapped == MAP_FAILED) {
                throw std::runtime_error("run_pull: mmap for verify failed at chunk " +
                                         std::to_string(chunk_index) + " - " + std::strerror(errno));
            }
            inputs.push_back({static_cast<const uint8_t*>(mapped), static_cast<size_t>(len)});
        }
        std::vector<std::array<uint8_t, HASH_SIZE>> digests(inputs.size());
        sha256_many(inputs, digests);

        std::vector<Result> results;
        results.reserve(chunk_indexes.size());
        for (size_t i = 0; i < chunk_indexes.size(); ++i) {
            results.push_back({chunk_indexes[i], digests[i] == meta_.chunk_hashes[chunk_indexes[i]]});
        }
        return results;
    }

    void run() {
        const size_t lanes = sha256_lanes();
        std::vector<uint32_t> batch;
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return closing_ || !pending_.empty(); });
            if (pending_.empty()) { return; } // closing, and nothing left to verify
            if (lanes > 1 && pending_.size() < lanes && !closing_) {
                wake_.wait_for(lock, VERIFY_BATCH_WAIT,
                               [&] { return closing_ || pending_.size() >= lanes; });
            }
            batch.clear();
            while (!pending_.empty() && batch.size() < lanes) {
                batch.push_back(pending_.front());
                pending_.pop_front();
            }
            if (batch.empty()) { continue; } // another thread took them during the wait
            lock.unlock();
            std::vector<Result> results;
            std::exception_ptr error;
            try {
                results = verify(batch);
            } catch (...) {
                error = std::current_exception();
            }
            lock.lock();
            if (error) {
                error_ = error;
                return;
            }
            finished_.insert(finished_.end(), results.begin(), results.end());
        }
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
        }
        wake_.notify_all();
        for (std::thread& worker : workers_) { worker.join(); }
        workers_.clear();
    }

    int fd_;
    const FileMeta& meta_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<uint32_t> pending_;
    std::chrono::steady_clock::time_point oldest_pending_; // used only when there is no thread
    std::vector<Result> finished_;
    std::exception_ptr error_;
    bool closing_ = false;
    std::vector<std::thread> workers_;
};

// true when out already holds the whole file: right size and every chunk hash; read-only
bool already_complete(const std::string& out, const FileMeta& meta) {
    const int raw_fd = ::open(out.c_str(), O_RDONLY);
    if (raw_fd < 0) { return false; }
    SocketFd fd{raw_fd};
    struct stat st{};
    if (::fstat(fd.get(), &st) < 0 || static_cast<uint64_t>(st.st_size) != meta.file_size) {
        return false;
    }
    std::vector<uint32_t> every_chunk(meta.chunk_count);
    std::iota(every_chunk.begin(), every_chunk.end(), 0u);
    return mismatched_on_disk(fd.get(), meta, every_chunk, true).empty();
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
        std::vector<uint32_t> recorded;
        for (uint32_t i = 0; i < file_meta.chunk_count; ++i) {
            if (!cm.needs_chunk(i)) { recorded.push_back(i); }
        }
        const std::vector<uint32_t> stale = mismatched_on_disk(fd.get(), file_meta, recorded, false);
        for (const uint32_t i : stale) { cm.mark_todo(i); }
        std::cout << "[pull] resuming partial download - " << recorded.size() - stale.size()
                  << " chunks verified on disk\n";
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

    ChunkVerifier verifier(fd.get(), file_meta, verify_threads());
    // a chunk counts once its hash matched; the record is saved each time results come in
    const auto record = [&](const std::vector<ChunkVerifier::Result>& results) {
        if (results.empty()) { return; }
        for (const ChunkVerifier::Result& result : results) {
            if (result.matches) {
                cm.mark_done(result.chunk_index);
            } else {
                cm.mark_todo(result.chunk_index);
                std::cerr << "\n[pull] hash mismatch at chunk " << result.chunk_index << "\n";
            }
        }
        PartialFile::save(out, file_meta.chunk_hashes[0], cm);
    };

    try {
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

            const auto [offset, chunk_len] = chunk_span(file_meta, chunk_index);

            // recv chunk data into output file, then hand it to the verifier
            t.recv_file(fd.get(), offset, chunk_len);
            verifier.submit(chunk_index);
            record(verifier.take_finished());
            verifier.rethrow_if_failed();

            std::cout << "[pull] received " << chunk_index + 1
                      << "/" << file_meta.chunk_count << "\r" << std::flush;
        }
    } catch (...) {
        // the connection broke: chunks already on disk still count, so a resume does not fetch them again
        try { record(verifier.finish()); } catch (...) {}
        throw;
    }
    record(verifier.finish());
    verifier.rethrow_if_failed();
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