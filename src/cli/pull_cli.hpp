//
// Created by benny on 4/20/26.
//

#pragma once

// Entry point of `embr pull` subcommand
// Parses args, detects token vs IP, optionally resolves via tracker, invokes run_pull
int run_pull_cli(int argc, char* argv[]);