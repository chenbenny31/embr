//
// Created by benny on 6/13/26.
//

#pragma once

// Entry point for `embr bench`
//   --role send: opens socket, set SO_SNDBUF (sweep), drives send_file
//   --role recv: connects, timestamps all metrics, reports HDR percentiles
int run_bench_cli(int argc, char* argv[]);