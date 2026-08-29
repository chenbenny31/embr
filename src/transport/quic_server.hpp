//
// Created by benny on 8/27/26.
//

#pragma once

#include "transport.hpp"
#include <cstdint>
#include <memory>
#include <string>

// Blocking UDP socket bound on port, ready for quic_accept
int quic_listen(uint16_t port);

// Block for 1st init on listen_fd, run handshake
// return a transport ready for send/recv
std::unique_ptr<Transport> quic_accept(int listen_fd,
                                       const std::string& cert_path,
                                       const std::string& key_path);