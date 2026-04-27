//
// Created by benny on 4/26/26.
//

#pragma once

#include <cstdint>
#include <string>

// JSON helpers, flat object only, no nesting nor array

// Parses value of string field: "key":"value", return empty if no such key
inline std::string json_get_str(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    const size_t key_pos = body.find(needle);
    if (key_pos == std::string::npos) { return {}; }
    const size_t val_start = key_pos + needle.length();
    const size_t val_end = body.find('"', val_start);
    if (val_end == std::string::npos) { return {}; }
    return body.substr(val_start, val_end - val_start);
}

// Parses value of an integer field: "key":value, return 0 if no such key
inline uint16_t json_get_u16(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    const size_t key_pos = body.find(needle);
    if (key_pos == std::string::npos) { return 0; }
    const size_t val_start = key_pos + needle.length();
    try {
        return static_cast<uint16_t>(std::stoul(body.substr(val_start)));
    } catch (...) {
        return 0;
    }
}