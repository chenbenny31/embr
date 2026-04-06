//
// Created by benny on 4/5/26.
//

#pragma once
#include "transport.hpp"
#include "udp_transport.hpp"
#include <memory>
#include <cstdint>

// Binds UDP socket on port, wait for peer HELLO, lock by connect()
std::unique_ptr<Transport> udp_bind(uint16_t port);