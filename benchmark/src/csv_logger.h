#pragma once

#include <fstream>
#include <iostream>
#include <string>

#include "key_generator.h"

class CsvLogger {
  public:
    CsvLogger(const std::string& filename, bool verbose)
        : filename_(filename), verbose_(verbose),
          output_(filename, std::fstream::out | std::fstream::app) {
        if (!output_.is_open()) {
            throw std::runtime_error("Can't open log file!");
        }
        if (output_.tellp() == 0) {
            print_header(output_);
        }
    }

    template <typename Container>
    void log(const std::string& run_name, const std::string& run_tag, unsigned threads,
             int payload_level, const KeyGenerator::ptr_t& gen, Container& cont, size_t iterations,
             std::chrono::duration<double> duration, bool log_to_console = true,
             std::ostream* out = nullptr) {
        if (out == nullptr) {
            out = &output_;
        }

        auto mem               = cont.memStats();
        auto perf              = cont.profileStats();
        auto throughput        = iterations / duration.count();
        auto thread_throughput = throughput / threads;
        auto element_overhead  = cont.currentOverheadMemory() / std::max(mem.count, 1lu);
        // clang-format off
        *out <<
             run_name << ", " <<
             run_tag << ", " <<
             gen->name() << ", " <<
             cont.name() << ", " <<
             mem.total_mem << ", " <<
             mem.capacity << ", " <<
             iterations << ", " <<
             duration.count() << ", " <<
             threads << ", " <<
             throughput << ", " <<
             thread_throughput << ", " <<
             element_overhead << ", " <<
             perf.find << ", " <<
             perf.insert << ", " <<
             perf.evict << ", " <<
             perf.head_accesses << ", " <<
             payload_level <<
             "\n";
        // clang-format on
        if (log_to_console) {
            if (verbose_) {
                verbose_log(run_name, run_tag, threads, payload_level, gen, cont, iterations,
                            duration, &std::cout);
            } else {
                log(run_name, run_tag, threads, payload_level, gen, cont, iterations, duration,
                    false, &std::cout);
            }
        }
    }

  private:
    void print_header(std::ostream& stream) {
        stream << "test_name, test_tag, generator, container, available_memory, capacity, "
                  "iterations, duration, threads, throughput, thread_throughput,"
                  "overhead_per_elem, find, insert, evict, head_access, payload_level\n";
    }

    template <typename Container>
    void verbose_log(const std::string& run_name, const std::string& run_tag, unsigned threads,
                     int payload_level, const KeyGenerator::ptr_t& gen, Container& cont,
                     size_t iterations, std::chrono::duration<double> duration,
                     std::ostream* out = nullptr) {
        const char* spacer = "     ";
        if (out == nullptr) {
            out = &output_;
        }

        auto mem               = cont.memStats();
        auto perf              = cont.profileStats();
        auto throughput        = iterations / duration.count();
        auto thread_throughput = throughput / threads;
        auto element_overhead  = cont.currentOverheadMemory() / std::max(mem.count, 1lu);
        *out << "Output file:     " << spacer << filename_ << "\n";
        *out << "Backend/name:    " << spacer << cont.name() << "/" << run_name;
        if (!run_tag.empty()) {
            *out << " (" << run_tag << ")";
        }
        *out << '\n';
        *out << "Memory/capacity: " << spacer << prettyPrintSize(mem.total_mem) << "/"
             << mem.capacity << "\n";
        *out << "Duration:        " << spacer << duration.count() << " s\n";
        *out << "Throughput:      " << spacer << thread_throughput << " op/s * " << threads << " = "
             << throughput << " op/s\n";
        *out << "Distribution:    " << spacer << gen->name() << "\n";
        *out << "F/I/E/HA:        " << spacer << (perf.find - perf.insert) / threads << "/"
             << perf.insert / threads << "/" << perf.evict / threads << "/"
             << perf.head_accesses / threads << "\n";
    }

    std::string  filename_;
    bool         verbose_;
    std::fstream output_;
};
