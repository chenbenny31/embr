//
// Created by benny on 4/25/26.
//

#pragma once

#include <cstdint>
#include <string>
#include <utility>

// Registers push under token for tracker_url
// upserts if token exists and throws on error
void tracker_register(const std::string& tracker_url,
                      const std::string& token,
                      uint16_t port);

// Resolves token to (sender_ip, sender_port)
std::pair<std::string, uint16_t> tracker_resolve(const std::string& tracker_url,
                                                 const std::string& token);

// Unregister with best effort, no throw
void tracker_unregister(const std::string& tracker_url,
                        const std::string& token) noexcept;