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
std::unique_ptr<Transport> quic_connect(const std::string& host, uint16_t port);