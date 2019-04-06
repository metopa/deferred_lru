#ifndef LSU3SHELL_METRICCOUNTER_H
#define LSU3SHELL_METRICCOUNTER_H

#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <omp.h>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

class MetricCounterStub {
  public:
    template <typename... ArgsT>
    MetricCounterStub(const ArgsT&...) {}

    void add(size_t value) {}

    void dump(const std::string& name) {}
};

class MetricCounterImpl {
  public:
    static constexpr size_t MAX_THREADS = 64;

    explicit MetricCounterImpl(std::string name) : name_(std::move(name)), dump_on_exit_(true) {}

    MetricCounterImpl() : dump_on_exit_(false) {}

    ~MetricCounterImpl() {
        if (dump_on_exit_) {
            dump(name_);
        }
    }

    void add(size_t value) {
        int thread_id = omp_get_thread_num();
        if (items_[thread_id].size() <= value) {
            items_[thread_id].resize(value + 1, 0);
        }
        items_[thread_id][value]++;
    }

    void dump(const std::string& name) {
        for (size_t thr = 1; thr < items_.size(); thr++) {
            if (items_[thr].size() > items_[0].size()) {
                items_[0].resize(items_[thr].size(), 0);
            }
            for (size_t i = 0; i < items_[thr].size(); i++) {
                items_[0][i] += items_[thr][i];
                items_[thr][i] = 0;
            }
        }

        size_t sum = 0;
        size_t cnt = 0;

        std::cerr << name_ << '\n';
        for (size_t i = 0; i < items_[0].size(); i++) {
            if (items_[0][i]) {
                sum += items_[0][i] * i;
                cnt += items_[0][i];
                std::cerr << '\t' << i << ":\t   " << items_[0][i] << '\n';
            }
        }
        std::cerr << "\ttotal: " << cnt << '\n';
        std::cerr << "\tmean:  " << double(sum) / cnt << '\n';
        std::cerr << "========================" << std::endl;
    }

  private:
    bool                                         dump_on_exit_;
    std::string                                  name_;
    std::array<std::vector<size_t>, MAX_THREADS> items_;
};

#endif // LSU3SHELL_METRICCOUNTER_H
