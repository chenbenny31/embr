//
// Created by benny on 3/23/26.
//

#include "hash.hpp"
#include <openssl/evp.h>
#include <stdexcept>

namespace {

// RAII wrapper for EVP_MD_CTX, guarantees free on throw
struct EvpCtx {
    EVP_MD_CTX* ctx;
    EvpCtx() : ctx(EVP_MD_CTX_new()) {
        if (!ctx) { throw std::runtime_error("EVP_MD_CTX_new failed"); }
    }
    ~EvpCtx() { EVP_MD_CTX_free(ctx); }
    EvpCtx(const EvpCtx&) = delete;
    EvpCtx& operator=(const EvpCtx&) = delete;
};

}

std::array<uint8_t, 32> sha256_buf(const uint8_t* data, size_t len) {
    EvpCtx evp;
    if (!EVP_DigestInit_ex(evp.ctx, EVP_sha256(), nullptr)) {
        throw std::runtime_error("sha256_buf: EVP_DigestInit_ex() failed");
    }
    if (!EVP_DigestUpdate(evp.ctx, data, len)) {
        throw std::runtime_error("sha256_buf: EVP_DigestUpdate() failed");
    }

    std::array<uint8_t, 32> digest{};
    unsigned int digest_len = 0;
    if (!EVP_DigestFinal_ex(evp.ctx, digest.data(), &digest_len)) {
        throw std::runtime_error("sha256_buf: EVP_DigestFinal_ex() failed");
    }

    return digest;
}