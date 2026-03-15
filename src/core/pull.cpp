//
// Created by benny on 3/15/26.
//

#include "pull.hpp"
#include "protocol.hpp"
#include "../transport/tcp_client.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

void run_pull(Transport& transport, const std::string& output_path) {
    // recv FILE_META
    Message meta_msg = recv_msg(transport);
    if (meta_msg.type != MsgType::FILE_META) {
        throw std::runtime_error("run_pull: expected FILE_META, got " +
                                 std::to_string(static_cast<uint8_t>(meta_msg.type)));
    }

    FileMeta meta = parse_filemeta(meta_msg);
    std::cout << "[pull] file=" << meta.filename
              << " size=" << meta.file_size << " bytes\n";

    // open output file
    const std::string& out = output_path.empty() ? meta.filename : output_path;
    std::ofstream file(out, std::ios::binary);
    if (!file) { throw std::runtime_error("run_pull: failed to open file " + out); }

    // recv file bytes, data plane bypassing Message
    Buffer buf(READ_BUF_SIZE);
    uint64_t total_recv = 0;

    while (total_recv < meta.file_size) {
        size_t to_recv = std::min(READ_BUF_SIZE, static_cast<size_t>(meta.file_size - total_recv));
        recv_exact(transport, buf.get(), to_recv);
        file.write(reinterpret_cast<char*>(buf.get()),
                   static_cast<std::streamsize>(to_recv));
        if (!file) { throw std::runtime_error("run_pull: failed to write " + out); }
        total_recv += to_recv;
        std::cout << "[pull]" << total_recv << "/" << meta.file_size << " bytes\r" << std::flush;
    }
    std::cout << "\n[pull] all bytes received\n";

    // send COMPLETE
    send_msg(transport, make_complete());
    std::cout << "[pull] transfer complete - saved to " << out << "\n";
}