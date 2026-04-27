//
// Created by benny on 3/1/26.
//

#pragma once

#include "transport.hpp"
#include <memory>
#include <cstdint>
#include <string>

// Client establishes a TCP connection to host::port, returns a TcpTransport
std::unique_ptr<Transport> tcp_connect(const std::string& host, uint16_t port);