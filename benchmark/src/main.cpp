#include <chrono>
#include <iostream>

#include "benchmark.h"

int main(int argc, char* argv[]) {
    const std::vector<std::string> backends{"hash", "lru", "concurrent", "deferred", "tbb", "hhvm"};

    BenchmarkApp app;

    bool cl_mode = (argc > 1);
    if (cl_mode) {
        auto rc = app.parse(argc, argv);
        if (rc) {
            return rc;
        }
    } else {
        app.log_file         = "../all.csv";
        app.run_name         = "timeout";
        app.run_info         = "2";
        app.backend          = "dummy";
        app.generator        = "normal";
        app.payload_level    = 20;
        app.threads          = 6;
        app.iterations       = 100000000;
        app.print_freq       = 10000;
        app.is_item_capacity = true;
        app.capacity         = 1024;
        app.pull_threshold   = 0.1;
        app.purge_threshold  = 0.1;
        app.verbose          = true;
        app.pull_threshold   = 0.5;
        app.purge_threshold  = 0.01;
    }

    app.run();

    return 0;
}
