//
// Created by benny on 3/14/26.
//

#pragma once
#include "../transport/transport.hpp"
#include <string>
#include <cstdint>

// Share side
void run_push(Transport& transport, const std::string& filepath);