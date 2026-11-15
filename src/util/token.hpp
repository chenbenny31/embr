#pragma once
#include "util/constants.hpp"
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

// Token: what push prints and pull is given
//   <content_hash>               16 hex - first 8 bytes of SHA-256 over the chunk hashes, the tracker key
//   <content_hash>-<cert_hash>   + 32 hex - SHA-256 prefix of the sender's TLS certificate
// the same token on every transport: one that carries TLS verifies cert_hash, plain TCP ignores it
inline constexpr size_t CERT_HASH_HEX = 32; // 128 bits

struct Token {
    std::string content_hash; // TOKEN_SIZE hex chars
    std::string cert_hash;    // empty, or CERT_HASH_HEX hex chars
};

inline bool is_lower_hex(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// nullopt when arg is not a token in either form
inline std::optional<Token> parse_token(const std::string& arg) {
    Token token;
    token.content_hash = arg.substr(0, TOKEN_SIZE);
    if (token.content_hash.size() != TOKEN_SIZE || !is_lower_hex(token.content_hash)) { return std::nullopt; }
    if (arg.size() == TOKEN_SIZE) { return token; }
    if (arg.size() != TOKEN_SIZE + 1 + CERT_HASH_HEX || arg[TOKEN_SIZE] != '-') { return std::nullopt; }
    token.cert_hash = arg.substr(TOKEN_SIZE + 1);
    if (!is_lower_hex(token.cert_hash)) { return std::nullopt; }
    return token;
}

// cert_sha256_hex: full 64-char digest of the sender's certificate, empty when there is none
inline std::string format_token(const std::string& content_hash, const std::string& cert_sha256_hex) {
    if (cert_sha256_hex.empty()) { return content_hash; }
    return content_hash + "-" + cert_sha256_hex.substr(0, CERT_HASH_HEX);
}
