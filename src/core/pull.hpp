//
// Created by benny on 3/15/26.
//

#pragma once

#include "../transport/transport.hpp"
#include <string>

// fetch side
void run_pull(Transport& tcp, Transport& udp, const std::string& output_path);