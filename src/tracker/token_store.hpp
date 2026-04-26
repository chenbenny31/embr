//
// Created by benny on 4/26/26.
//

#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

struct Record {
    std::string sender_ip;
    uint16_t sender_port;
    std::chrono::steady_clock::time_point registered_at;
};

class TokenStore {
public:
    explicit TokenStore(std::chrono::minutes ttl);

    // Insert or replace record for token, reset registered_at
    void upsert(const std::string& token, Record record);

    // Return Record if token exists and alive, lazy evict expired ones
    std::optional<Record> resolve(const std::string& token);

    // Remove token, return true if exist
    bool remove(const std::string& token);

    // Evicts all expired record, return count
    // called by background thread every 60s
    size_t sweep_expired();

private:
    std::unordered_map<std::string, Record> token_store_;
    mutable std::mutex mtx_;
    std::chrono::minutes ttl_;
};