//
// Created by benny on 4/20/26.
//

#include "pull_cli.hpp"

#include "core/pull.hpp"
#include "tracker/tracker_client.hpp"
#include "transport/quic_client.hpp"
#include "transport/tcp_client.hpp"
#include "util/config_tracker.hpp"
#include "util/constants.hpp"
#include "util/token.hpp"

#include <arpa/inet.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void print_pull_usage() {
    std::cerr << "Usage: embr pull <token> [--ip ADDR] [--port PORT] [--tracker URL] [--out PATH]\n"
              << "       embr pull <ip>    [--port PORT] [--out PATH]   (sender not verified)\n"
              << "\n"
              << "  <token>         the token push printed; it carries the sender's certificate\n"
              << "                  hash, which QUIC verifies and plain TCP ignores\n"
              << "  --ip ADDR       sender address: connect directly, no tracker lookup\n"
              << "  --port PORT     sender port for direct mode (default 10007)\n"
              << "  --tracker URL   tracker URL; overrides EMBR_TRACKER env var\n"
              << "  --out PATH      output file path (default filename from sender)\n"
              << "  --transport T   tcp (default) or quic\n";
}

bool is_ipv4(const std::string& arg) {
    struct in_addr addr{};
    return ::inet_pton(AF_INET, arg.c_str(), &addr) == 1;
}

}

int run_pull_cli(int argc, char* argv[]) {
    if (argc < 2) {
        print_pull_usage();
        return 1;
    }

    const std::string arg = argv[1];
    uint16_t port = EMBR_PORT;
    std::string tracker_url;
    std::string out_path;
    std::string transport = "tcp";
    std::string direct_ip; // --ip: where the sender is, when no tracker is asked

    for (int i = 2; i < argc; i++) {
        const std::string flag = argv[i];
        if (flag == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (flag == "--ip" && i + 1 < argc) {
            direct_ip = argv[++i];
        } else if (flag == "--tracker" && i + 1 < argc) {
            tracker_url = argv[++i];
        } else if (flag == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (flag == "--help") {
            print_pull_usage();
            return 0;
        } else if (flag == "--transport" && i + 1 < argc) {
            transport = argv[++i];
        } else {
            std::cerr << "embr pull: unknown flag: " << flag << "\n";
            print_pull_usage();
            return 1;
        }
    }

    if (transport != "tcp" && transport != "quic") {
        std::cerr << "embr pull: --transport must be tcp or quic\n";
        return 1;
    }

    tracker_url = resolve_tracker_url(tracker_url);

    const std::optional<Token> token = parse_token(arg);
    const std::string cert_hash = token ? token->cert_hash : std::string{};
    if (!direct_ip.empty() && (!token || !is_ipv4(direct_ip))) {
        std::cerr << "[error] --ip takes an IPv4 address and goes with a token: "
                  << "embr pull <token> --ip <address>\n";
        return 1;
    }

    try {
        std::string sender_ip;
        uint16_t sender_port;

        if (is_ipv4(arg) || !direct_ip.empty()) { // direct mode: bare address, or token + --ip
            sender_ip = direct_ip.empty() ? arg : direct_ip;
            sender_port = port;
            std::cout << "[pull] direct mode: " << sender_ip
                      << ":" << sender_port << "\n";

        } else if (token) { // tracker mode
            if (tracker_url.empty()) {
                std::cerr << "[error] token provided but no tracker configured\n"
                          << "        set EMBR_TRACKER or pass --tracker URL\n";
                return 1;
            }
            std::cout << "[pull] resolving token: " << arg
                      << " tracker: " << tracker_url << "\n";
            auto [resolved_ip, resolved_port] = tracker_resolve(tracker_url, token->content_hash);
            sender_ip = resolved_ip;
            sender_port = resolved_port;
            std::cout << "[pull] resolved to " << sender_ip
                      << ":" << sender_port << "\n";

        } else {
            std::cerr << "[error] '" << arg
                      << "' is neither a valid IPv4 address nor a valid token\n";
            return 1;
        }

        if (transport == "quic" && cert_hash.empty()) {
            std::cerr << "[warn] sender certificate not verified - pull with the token push printed\n";
        }
        auto conn = (transport == "quic") ? quic_connect(sender_ip, sender_port, cert_hash)
                                          : tcp_connect(sender_ip, sender_port);
        std::cout << "[pull] connect to " << sender_ip
                  << ":" << sender_port << "\n";
        run_pull(*conn, out_path);

    } catch (const std::exception& ex) {
        std::cerr << "[error] " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
