//
// Created by benny on 4/26/26.
//

#include "token_store.hpp"
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

TokenStore::TokenStore(std::chrono::minutes ttl)
    : ttl_(ttl) {}

void TokenStore::upsert(const std::string& token, Record record) {
    record.registered_at = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mtx_);
    token_store_[token] = std::move(record);
}

std::optional<Record> TokenStore::resolve(const std::string& token) {
    std::lock_guard<std::mutex> lock(mtx_);

    // lazy evict: sweep on each resolve call
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> expired_tokens;
    for (const auto& [key, record] : token_store_) {
        if (now - record.registered_at > ttl_) {
            expired_tokens.push_back(key);
        }
    }
    for (const auto& t : expired_tokens) {
        token_store_.erase(t);
    }

    const auto it = token_store_.find(token);
    if (it == token_store_.end()) { return std::nullopt; }
    return it->second;
}

bool TokenStore::remove(const std::string& token) {
    std::lock_guard<std::mutex> lock(mtx_);
    return token_store_.erase(token) > 0;
}

size_t TokenStore::sweep_expired() {
    std::lock_guard<std::mutex> lock(mtx_);
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> expired_tokens;
    for (const auto& [key, record] : token_store_) {
        if (now - record.registered_at > ttl_) {
            expired_tokens.push_back(key);
        }
    }
    for (const auto& t : expired_tokens) {
        token_store_.erase(t);
    }
    return expired_tokens.size();
}