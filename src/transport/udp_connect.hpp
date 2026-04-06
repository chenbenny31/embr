//
// Created by benny on 4/5/26.
//

#pragma once
#include "transport.hpp"
#include "udp_transport.hpp"
#include <memory>
#include <string>
#include <cstdint>

// Creates a connected UDP socket on host:port, performs HELLO to lock peer addr
std::unique_ptr<Transport> udp_connect(const std::string& host, uint16_t port);