//
// Created by benny on 4/27/26.
//

#include "trust_cli.hpp"
#include "util/config_tracker.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void print_trust_usage() {
    std::cerr << "Usage: embr trust <url>   - save tracker URL\n"
              << "       embr trust --list  - print current trusted trackers\n"
              << "       embr trust --clear - remove saved trackers\n"
              << "\n"
              << "Resolution order:\n"
              << "  1. --tracker flag\n"
              << "  2. EMBR_TRACKER env var\n"
              << "  3. ~/.config/embr/tracker (set by embr trust)\n"
              << "  4. empty — direct IP mode only\n";
}

}

int run_trust_cli(int argc, char* argv[]) {
    if (argc < 2) {
        print_trust_usage();
        return 1;
    }

    const std::string arg = argv[1];
    if (arg == "--list") {
        const std::string url = read_tracker_url();
        if (url.empty()) {
            std::cout << "[trust] no tracker configured\n";
        } else {
            std::cout << "[trust] trusted tracker: " << url << "\n";
        }
        return 0;
    }

    if (arg == "--clear") {
        const std::filesystem::path config_path = get_config_path();
        if (std::filesystem::exists(config_path)) {
            std::filesystem::remove(config_path);
            std::cout << "[trust] tracker config cleared\n";
        } else {
            std::cout << "[trust] no tracker configured\n";
        }
        return 0;
    }

    if (arg == "--help") {
        print_trust_usage();
        return 0;
    }

    const std::string url = arg;
    write_tracker_url(url);
    std::cout << "[trust] saved tracker: " << url << "\n";
    return 0;
}