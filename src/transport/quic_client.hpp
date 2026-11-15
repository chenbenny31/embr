//
// Created by benny on 6/20/26.
//

#pragma once

#include "transport.hpp"
#include <cstdint>
#include <memory>
#include <string>

// Establish a QUIC connection to host:port over UDP with TLS 1.3 handshake
// open the single bidi stream, returns a transport ready for send/recv
// cert_hash: hex prefix of the SHA-256 the sender's certificate must have; empty = not verified
std::unique_ptr<Transport> quic_connect(const std::string& host, uint16_t port,
                                        const std::string& cert_hash = {});