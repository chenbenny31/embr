//
// Created by benny on 4/20/26.
//

#include "push_cli.hpp"
#include "core/hash.hpp"
#include "core/push.hpp"
#include "tracker/tracker_client.hpp"
#include "transport/tcp_server.hpp"
#include "transport/quic_server.hpp"
#include "util/socket_fd.hpp"
#include "util/config_tracker.hpp"
#include "util/token.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void print_push_usage() {
    std::cerr << "Usage: embr push <file> [--port PORT] [--tracker URL] [--ip IP]\n"
              << "\n"
              << "  --port PORT     listen port (default: 10007)\n"
              << "  --tracker URL   tracker URL; overrides EMBR_TRACKER env var\n"
              << "  --ip IP         override sender IP under NAT\n"
              << "  --transport T   tcp (default) or quic\n"
              << "  --cert PATH     QUIC server cert (default: ~/.config/embr/cert.pem)\n"
              << "  --key PATH      QUIC server key (default: ~/.config/embr/key.pem)\n";
}

// Derive 16 hex char token from file (deterministic), first 8 bytes from SHA256 hashes
std::string derive_token(const std::vector<std::array<uint8_t, HASH_SIZE>>& chunk_hashes) {
    std::vector<uint8_t> concat;
    concat.reserve(chunk_hashes.size() * HASH_SIZE);
    for (const auto& hash : chunk_hashes) {
        concat.insert(concat.end(), hash.begin(), hash.end());
    }
    const auto digest = sha256_buf(concat.data(), concat.size());

    std::ostringstream oss;
    for (size_t i = 0; i < 8; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return oss.str();
}

// default QUIC cert/key live beside the trusted-tracker file; install.sh creates them
std::string default_cred_path(const char* name) {
    const std::filesystem::path dir = get_config_path().parent_path();
    return dir.empty() ? std::string(name) : (dir / name).string();
}

// cert hash for the token; plain TCP never loads the cert, so there a missing one is not an error
std::string token_cert_hash(const std::string& cert_path, bool required) {
    try {
        return quic_cert_hash(cert_path);
    } catch (const std::exception&) {
        if (required) { throw; }
        return {};
    }
}

// checked before hashing or listening: a missing cert must not surface at the first packet
bool quic_creds_readable(const std::string& cert_path, const std::string& key_path) {
    if (::access(cert_path.c_str(), R_OK) == 0 && ::access(key_path.c_str(), R_OK) == 0) {
        return true;
    }
    const std::string dir = std::filesystem::path(cert_path).parent_path().string();
    std::cerr << "[error] QUIC needs a cert/key pair, not readable: "
              << cert_path << " / " << key_path << "\n"
              << "        create a self-signed pair:\n        "
              << (dir.empty() ? "" : "mkdir -p " + dir + " && ")
              << "openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj /CN=embr"
              << " -keyout " << key_path << " -out " << cert_path << "\n";
    return false;
}

}

int run_push_cli(int argc, char* argv[]) {
    if (argc < 2) {
        print_push_usage();
        return 1;
    }

    const std::string file_path = argv[1];
    uint16_t port = EMBR_PORT;
    std::string tracker_url;
    std::string sender_ip;
    std::string transport = "tcp";
    std::string cert_path = default_cred_path("cert.pem");
    std::string key_path = default_cred_path("key.pem");

    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (flag == "--tracker" && i + 1 < argc) {
            tracker_url = argv[++i];
        } else if (flag == "--ip" && i + 1 < argc) {
            sender_ip = argv[++i];
        } else if (flag == "--help") {
            print_push_usage();
            return 0;
        } else if (flag == "--transport" && i + 1 < argc) {
            transport = argv[++i];
        } else if (flag == "--cert" && i + 1 < argc) {
            cert_path = argv[++i];
        } else if (flag == "--key" && i + 1 < argc) {
            key_path = argv[++i];
        } else {
            std::cerr << "embr push: unknown flag: " << flag << "\n";
            print_push_usage();
            return 1;
        }
    }

    if (transport != "tcp" && transport != "quic") {
        std::cerr << "embr push: --transport must be tcp or quic\n";
        return 1;
    }
    const bool use_quic = (transport == "quic");
    if (use_quic && !quic_creds_readable(cert_path, key_path)) { return 1; }

    tracker_url = resolve_tracker_url(tracker_url);
    if (!tracker_url.empty()) {
        std::cout << "[push] tracker: " << tracker_url << "\n";
    }

    try {
        FileMeta file_meta = precompute_meta(file_path);
        SocketFd file_fd{::open(file_path.c_str(), O_RDONLY)};
        if (file_fd.get() < 0) {
            throw std::runtime_error("embr push: failed to open file: " + file_path +
                                     " - " + std::strerror(errno));
        }

        const std::string token = derive_token(file_meta.chunk_hashes);
        // what pull is given, the same on every transport: content_hash, plus cert_hash when
        // this sender has a certificate; plain TCP carries cert_hash but cannot check it
        const std::string shown_token = format_token(token, token_cert_hash(cert_path, use_quic));

        SocketFd listen_fd = use_quic ? SocketFd{quic_listen(port)} : SocketFd{tcp_listen(port)};
        std::cout << "[push] listening on port " << port
                  << " (" << transport << ")\n";
        std::cout << "[push] token: " << shown_token << std::endl; // flushed: a script reads it from a pipe

        // register with tracker after listen_fd is bound: port open before pull connection
        if (!tracker_url.empty()) {
            tracker_register(tracker_url, token, port); // v0.6: sender_ip is shadow, only for NAT
            if (!sender_ip.empty()) {
                std::cout << "[push] sender ip: " << sender_ip << "\n";
            }
        }

        // quic_accept owns the socket from here, on success and on every error path
        auto conn = use_quic ? quic_accept(listen_fd.release(), cert_path, key_path)
                             : tcp_accept(listen_fd.get());
        std::cout << "[push] connection established\n";

        run_push(*conn, std::move(file_fd), std::move(file_meta));

        if (!tracker_url.empty()) {
            tracker_unregister(tracker_url, token);
        }

    } catch (const std::exception& ex) {
        std::cerr << "[error] " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
