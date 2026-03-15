//
// Created by benny on 3/14/26.
//

#include "push.hpp"
#include "protocol.hpp"
#include "../transport/tcp_server.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

void run_push(Transport& transport, const std::string& filepath) {
    // open file
    std::ifstream file(filepath, std::ios::binary);
    if (!file) { throw std::runtime_error("run_push: cannot open file: " + filepath); }

    uint64_t file_size = std::filesystem::file_size(filepath);
    if (file_size == 0) { throw std::runtime_error("run_push: file is empty: " + filepath); }

    std::string filename = std::filesystem::path(filepath).filename().string();

    // send FILE_META
    send_msg(transport, make_filemeta(FileMeta{filename, file_size}));
    std::cout << "[push] sent FILE_META - file=" << filename
              << " size=" << file_size << " bytes\n";

    // send file bytes - data plane bypassing Message
    Buffer buf(READ_BUF_SIZE);
    uint64_t total_sent = 0;

    while (total_sent < file_size) {
        size_t to_read = std::min(READ_BUF_SIZE, static_cast<size_t>(file_size - total_sent));
        file.read(reinterpret_cast<char*>(buf.get()), static_cast<std::streamsize>(to_read));
        auto done_read = static_cast<size_t>(file.gcount());
        if (done_read == 0) { throw std::runtime_error("run_push: unexpected EOF"); }
        size_t bytes_read = static_cast<size_t>(done_read);

        send_exact(transport, buf.get(), bytes_read);
        total_sent += bytes_read;

        std::cout << "[push] total_sent=" << total_sent << "/" << file_size << " bytes\r" << std::flush;
    }
    std::cout << "\n[push] all bytes sent\n";

    // wait for COMPLETE
    Message ret = recv_msg(transport);
    if (ret.type != MsgType::COMPLETE) {
        throw std::runtime_error("run_push: expected COMPLETE, got " +
                                 std::to_string(static_cast<uint8_t>(ret.type)));
    }

    std::cout << "[push] transfer complete\n";
}