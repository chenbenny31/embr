//
// Created by benny on 4/27/26.
//

#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

// Return path to tracker config file: ~/.config/embr/tracker
inline std::filesystem::path get_config_path() {
    const char* home = ::getenv("HOME");
    if (!home || home[0] == '\0') { return {}; }
    return std::filesystem::path(home) / ".config" / "embr" / "tracker";
}

// Read saved tracker URL from ~/.config/embr/tracker, return empty string if not exist
inline std::string read_tracker_url() {
    const std::filesystem::path config_path = get_config_path();
    if (config_path.empty() || !std::filesystem::exists(config_path)) { return {}; }
    std::ifstream file(config_path);
    std::string url;
    std::getline(file, url);
    return url;
}

// Write tracker URL to ~/.config/embr/tracker, create dir if not exist
inline void write_tracker_url(const std::string& url) {
    const std::filesystem::path config_path = get_config_path();
    if (config_path.empty()) {
        throw std::runtime_error("write_tracker_url: HOME not set");
    }
    std::filesystem::create_directories(config_path.parent_path());
    std::ofstream file(config_path);
    file << url << "\n";
}

// Resolve tracker URL from all sources with priority:
//   1. --tracker flag
//   2. EMBR_TRACKER env var
//   3. ~/.config/embr/tracker file
//   4. empty string (direct mode only)
inline std::string resolve_tracker_url(const std::string& flag_value) {
    if (!flag_value.empty()) { return flag_value; }
    const char* env = ::getenv("EMBR_TRACKER");
    if (env && env[0] != '\0') { return std::string(env); }
    return read_tracker_url();
}