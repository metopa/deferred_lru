#pragma once

#include <string>

#include "CLI11.hpp"

struct BenchmarkApp {
    CLI::App    app;
    std::string log_file;
    std::string run_name;
    std::string run_info;
    std::string generator;
    std::string backend;
    int         payload_level;
    unsigned    threads;
    size_t      iterations;
    bool        limit_max_key;
    bool        is_item_capacity;
    size_t      capacity;
    double      pull_threshold;
    double      purge_threshold;
    bool        verbose;
    size_t      print_freq;
    int         time_limit;

    BenchmarkApp();

    static const char* help();

    int parse(int argc, char** argv);

    void run();
};
