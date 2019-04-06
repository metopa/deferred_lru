#pragma once

#include <cassert>
#include <optional>

#include "config.h"
#include "container_base.h"
#include "utility.h"

#include <thread-safe-lru/scalable-cache.h>

#include <chrono>
#include <mutex>

template <typename Config>
class HhvmLRU : public ContainerBase<Config, HhvmLRU<Config>, false> {

    using base    = ContainerBase<Config, HhvmLRU<Config>, false>;
    using config  = Config;
    using key_t   = typename config::key_t;
    using value_t = typename config::value_t;
    using impl_t  = tstarling::ThreadSafeScalableCache<key_t, value_t>;

  public:
    /// initializes a HashFixed that stores size objects. Subsequently added object will cause an
    /// undefined behavior
    HhvmLRU(size_t capacity, bool is_item_capacity) { allocateMemory(capacity, is_item_capacity); }

    ~HhvmLRU() { releaseMemory(); }

    static const char* name() { return "HHVM"; }

    decltype(auto) profileStats() const { return profile_stats_.getSlice(); }

    size_t currentOverheadMemory() const {
        return static_cast<size_t>(this->max_element_count_ *
                                   (elementSize() - sizeof(key_t) - sizeof(value_t)));
    }

    static double elementSize() {
        using list_node_t = struct {
            key_t x;
            void* y;
            void* z;
        };
        using map_node_t = struct {
            value_t x;
            void*   y;
        };

        using map_t = tbb::concurrent_hash_map<key_t, map_node_t>;
        //TODO Fix size
        return sizeof(typename map_t::value_type) +
               sizeof(list_node_t);
    }

    void allocateMemory(size_t capacity, bool is_item_capacity) {
        this->init(capacity, is_item_capacity);
        impl_.emplace(this->max_element_count_);
        profile_stats_.reset();
    }

    /// calls the policy on all the objects in the cache
    void releaseMemory() { impl_.reset(); }

    template <typename Producer, typename Consumer>
    void consumeCachedOrCompute(const key_t& key, const Producer& producer, Consumer& consumer) {
        this->profile_stats_.find++;
        typename impl_t::ConstAccessor ac;
        if (impl_->find(ac, key)) {
            consumer = *ac;
            return;
        }
        auto x = producer();
        this->profile_stats_.insert++;
        impl_->insert(key, x);
        consumer = x;
    }

  private:
    static size_t memSizeForElements(size_t count) {
        return size_t(std::ceil(base::estimatedElementSize() * count));
    }

    std::optional<impl_t>            impl_;
    typename config::profile_stats_t profile_stats_;
};
