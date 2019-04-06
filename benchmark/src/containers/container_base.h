#pragma once

#include "config.h"
#include <atomic>
#include <cstddef>

template <typename Config, typename CrtpDerived, bool UseAtomics>
class ContainerBase {
  public:
    using key_t   = typename Config::key_t;
    using value_t = typename Config::value_t;
    using idx_t   = typename Config::idx_t;
    template <typename T>
    using atomic_t = std::conditional_t<UseAtomics, std::atomic<T>, T>;

  protected:
    explicit ContainerBase(size_t capacity = 0, bool is_item_capacity = false) {
        init(capacity, is_item_capacity);
    }

    void init(size_t capacity, bool is_item_capacity) {
        max_element_count_ = is_item_capacity ? capacity : maxElementCountForCapacity(capacity);
        total_mem_available_ =
            is_item_capacity ? (size_t)(estimatedElementSize() * capacity) : capacity;
        current_element_count_ = 0;
    }

  private:
    static double elementSizeCrtp() { return CrtpDerived::elementSize(); }

    size_t currentOverheadMemoryCrtp() const {
        return ((CrtpDerived*)this)->currentOverheadMemory();
    }

  public:
    static double estimatedElementSize() { return elementSizeCrtp(); }

    static double elementSize(const key_t& key, const value_t& value) { return elementSizeCrtp(); }

    static size_t maxElementCountForCapacity(size_t capacity) {
        auto s = estimatedElementSize();
        return static_cast<size_t>(capacity / s);
    }

    size_t capacity() const { return max_element_count_; }

    size_t size() const { return current_element_count_; }

    MemStats memStats() const {
        MemStats s{};
        s.count              = current_element_count_;
        s.capacity           = max_element_count_;
        s.total_mem          = total_mem_available_;
        s.used_mem           = elementSizeCrtp() * current_element_count_;
        s.total_overhead_mem = currentOverheadMemoryCrtp();
        return s;
    }

  protected:
    size_t max_element_count_;
    size_t total_mem_available_;

    atomic_t<idx_t> current_element_count_;
};
