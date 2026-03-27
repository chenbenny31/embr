//
// Created by benny on 3/23/26.
//

#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>

// SHA256 of an in-memory buffer for per-chunk verification
std::array<uint8_t, 32> sha256_buf(const uint8_t* data, size_t len);