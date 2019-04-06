#pragma once

#include <atomic>
#include <ostream>
#include <string>

#include "metric_counter.h"

template <typename IntT>
struct FakeInt {
    FakeInt() {}

    FakeInt(IntT) {}

    template <typename Any>
    FakeInt& operator=(const Any&) {
        return *this;
    };

    template <typename Any>
    FakeInt& operator+=(const Any&) {
        return *this;
    };

    FakeInt& operator++() { return *this; }

    FakeInt& operator++(int) { return *this; }

    operator IntT() const { return 0; }

    IntT load() const { return 0; }
};

struct ProfileStatsSlice {
    size_t find;
    size_t insert;
    size_t head_accesses;
    size_t evict;
    bool   enabled;

    void print(std::ostream& out, const char* prefix = "") {
        if (enabled) {
            out << prefix << "find:          " << find << "\n"
                << prefix << "insert:        " << insert << "\n"
                << prefix << "head accesses: " << head_accesses << "\n"
                << prefix << "evict:         " << evict << "\n";
        }
    }

    ProfileStatsSlice& operator+=(const ProfileStatsSlice& other) {
        find += other.find;
        insert += other.insert;
        head_accesses += other.head_accesses;
        evict += other.evict;
        return *this;
    }
};

template <bool Enable>
struct ProfileStats {
    using int_t = std::conditional_t<Enable, std::atomic<size_t>, FakeInt<size_t>>;

    int_t find;
    int_t insert;
    int_t head_accesses;
    int_t evict;

    ProfileStats() { reset(); }

    void reset() {
        find          = 0;
        insert        = 0;
        head_accesses = 0;
        evict         = 0;
    }

    ProfileStatsSlice getSlice() const { return {find, insert, head_accesses, evict, Enable}; }
};

inline std::string prettyPrintSize(size_t size) {
    const char* units[]    = {" b", " Kb", " Mb", " Gb", " Tb"};
    int         unit_index = 0;

    if (size < 1 << 10) {
        return std::to_string(size) + units[unit_index];
    }

    while (size >= 1 << 20) {
        size >>= 10;
        unit_index++;
    }

    char buff[100];
    snprintf(buff, sizeof(buff), "%.3f%s", size / 1024., units[unit_index + 1]);

    return {buff};
}

template <typename Number>
std::string prettyPrintRatio(Number x, Number total) {
    if (total == 0) {
        return "[0%]";
    }
    return '[' + std::to_string(int(x * 100 / total)) + "%]";
}

struct MemStats {
    size_t count;
    size_t capacity;
    size_t total_mem;
    size_t used_mem;
    size_t total_overhead_mem;

    void print(std::ostream& out, const char* prefix = "") const {
        out << prefix << "total memory usage:   " << prettyPrintSize(used_mem) << '/'
            << prettyPrintSize(total_mem) << ' ' << prettyPrintRatio(used_mem, total_mem) << '\n';
        out << prefix << "elements:             " << count << "/" << capacity << " "
            << prettyPrintRatio(count, capacity) << "\n";
        if (count) {
            auto overhead  = total_overhead_mem / count;
            auto elem_size = used_mem / count;
            out << prefix << "overhead per element: " << prettyPrintSize(overhead) << '/'
                << prettyPrintSize(elem_size) << ' '
                << prettyPrintRatio(total_overhead_mem, used_mem) << std::endl;
        }
    }

    MemStats& operator+=(const MemStats& other) {
        count += other.count;
        capacity += other.capacity;
        total_mem += other.total_mem;
        used_mem += other.used_mem;
        total_overhead_mem += other.total_overhead_mem;
        return *this;
    }
};

class OpenMPLock {
    omp_lock_t writelock;

  public:
    OpenMPLock() { omp_init_lock(&writelock); }

    ~OpenMPLock() { omp_destroy_lock(&writelock); }

    void lock() { omp_set_lock(&writelock); }

    void unlock() { omp_unset_lock(&writelock); }

    bool try_lock() { return static_cast<bool>(omp_test_lock(&writelock)); }
};
