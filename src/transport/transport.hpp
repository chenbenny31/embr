//
// Created by benny on 3/12/26.
//

#pragma once
#include <cstdint>
#include <sys/types.h>

// Abstract transport interface
// send/recv are best-effort and possibly partial
// all bytes are transferred in one call via send_exact/recv_exact looping over Transport& in protocol.hpp
// Factory convention: tcp/quic -> std::unique_ptr<Transport>
class Transport {
public:
    // Returns bytes transferred, -1 on error, 0 on close
    virtual ssize_t send(const uint8_t* buf, size_t len) = 0;
    virtual ssize_t recv(uint8_t* buf, size_t len) = 0;
    virtual ~Transport() = default;
};