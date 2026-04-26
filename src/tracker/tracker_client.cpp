//
// Created by benny on 4/25/26.
//

#include "tracker_client.hpp"
#include <httplib.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

// Parses value of string field: "key":"value", return empty if no such key
std::string json_get_str(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    const size_t key_pos = body.find(needle);
    if (key_pos == std::string::npos) { return {}; }
    const size_t val_start = key_pos + needle.size();
    const size_t val_end = body.find('"', val_start);
    if (val_end == std::string::npos) { return {}; }
    return body.substr(val_start, val_end - val_start);
}

// Parses value of an integer field: "key":value, return 0 if no such key
uint16_t json_get_u16(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    const size_t key_pos = body.find(needle);
    if (key_pos == std::string::npos) { return 0; }
    const size_t val_start = key_pos + needle.size();
    return static_cast<uint16_t>(std::stoul(body.substr(val_start)));
}

// Splits tracker_url into (host, port, base_path)
// e.g. "http://plurb.org:10007" -> ("plurb.org", 10007, "")
struct ParsedUrl {
    std::string host;
    int port;
};
ParsedUrl parse_url(const std::string& tracker_url) {
    const std::string prefix = "http://";
    size_t start = 0;
    if (tracker_url.substr(0, prefix.size()) == prefix) {
        start = prefix.size();
    }
    const size_t colon_pos = tracker_url.rfind(':');
    if (colon_pos == std::string::npos || colon_pos < start) {
        throw std::runtime_error("tracker_client: malformed tracker URL: " + tracker_url);
    }
    return {
        tracker_url.substr(start, colon_pos - start),
        std::stoi(tracker_url.substr(colon_pos + 1))
    };
}

}

void tracker_register(const std::string& tracker_url,
                      const std::string& token,
                      uint16_t port) {
    auto [host, http_port] = parse_url(tracker_url);
    httplib::Client client(host, http_port);
    client.set_connection_timeout(10);
    client.set_read_timeout(10);

    const std::string body = "{\"token\":\"" + token + "\","
                             "\"port\":" + std::to_string(port) + "}";
    auto result = client.Post("/register", body, "application/json");
    if (!result) {
        throw std::runtime_error("tracker_register: connection failed to " + tracker_url);
    }
    if (result->status != 200) {
        throw std::runtime_error("tracker_register: HTTP " +
                                 std::to_string(result->status) +
                                 " from " + tracker_url);
    }
}

std::pair<std::string, uint16_t> tracker_resolve(const std::string& tracker_url,
                                                 const std::string& token) {
    auto [host, http_port] = parse_url(tracker_url);
    httplib::Client client(host, http_port);
    client.set_connection_timeout(10);
    client.set_read_timeout(10);

    auto result = client.Get("/resolve/" + token);
    if (!result) {
        throw std::runtime_error("tracker_resolve: connection failed to " + tracker_url);
    }
    if (result->status == 404) {
        throw std::runtime_error("tracker_resolve: token not found: " + token);
    }
    if (result->status != 200) {
        throw std::runtime_error("tracker_resolve: HTTP "  +
                                 std::to_string(result->status) +
                                 " from " + tracker_url);
    }

    const std::string sender_ip = json_get_str(result->body, "sender_ip");
    const uint16_t sender_port = json_get_u16(result->body, "port");
    if (sender_ip.empty() || sender_port == 0) {
        throw std::runtime_error("tracker_resolve: malformed response: " + result->body);
    }
    return {sender_ip, sender_port};
}

void tracker_unregister(const std::string& tracker_url,
                        const std::string& token) noexcept {
    try {
        auto [host, http_port] = parse_url(tracker_url);
        httplib::Client client(host, http_port);
        client.set_connection_timeout(10);
        client.set_read_timeout(10);
        client.Delete("/unregister/" + token);
    } catch (...) {

    }
}