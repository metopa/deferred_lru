#pragma once

#include <cassert>
#include <optional>

#include "config.h"
#include "container_base.h"
#include "utility.h"

#include <tbb/concurrent_lru_cache.h>

#include <chrono>
#include <mutex>

template <typename Config>
class TbbLRU : public ContainerBase<Config, TbbLRU<Config>, false> {

    using base    = ContainerBase<Config, TbbLRU<Config>, false>;
    using config  = Config;
    using key_t   = typename config::key_t;
    using value_t = typename config::value_t;
    using impl_t  = tbb::concurrent_lru_cache<key_t, value_t>;

  public:
    /// initializes a HashFixed that stores size objects. Subsequently added object will cause an
    /// undefined behavior
    TbbLRU(size_t capacity, bool is_item_capacity) { allocateMemory(capacity, is_item_capacity); }

    ~TbbLRU() { releaseMemory(); }

    static const char* name() { return "TBB"; }

    decltype(auto) profileStats() const { return profile_stats_.getSlice(); }

    size_t currentOverheadMemory() const {
        return this->max_element_count_ * (elementSize() - sizeof(key_t) - sizeof(value_t));
    }

    static double elementSize() {
        return sizeof(typename impl_t::map_storage_type::value_type) +
               sizeof(std::_Rb_tree_node_base) +
               sizeof(typename impl_t::lru_list_type::value_type) + sizeof(void*) * 2;
    }

    void allocateMemory(size_t capacity, bool is_item_capacity) {
        this->init(capacity, is_item_capacity);
        impl_.emplace(nullptr, this->max_element_count_);
        profile_stats_.reset();
    }

    /// calls the policy on all the objects in the cache
    void releaseMemory() { impl_.reset(); }

    template <typename Producer, typename Consumer>
    void consumeCachedOrCompute(const key_t& key, const Producer& producer, Consumer& consumer) {
        this->profile_stats_.find++;
        auto proxied_producer = [&] {
            this->profile_stats_.insert++;
            return producer();
        };
        auto handler = impl_->getOrCalculate(key, proxied_producer);
        consumer     = handler.value();
    }

  private:
    static size_t memSizeForElements(size_t count) {
        return size_t(std::ceil(base::estimatedElementSize() * count));
    }

    std::optional<impl_t>            impl_;
    typename config::profile_stats_t profile_stats_;
};
