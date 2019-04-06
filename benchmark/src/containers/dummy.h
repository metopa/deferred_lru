#pragma once

#include "common.h"
#include "config.h"
#include "container_base.h"
#include "utility.h"

template <typename Config>
class DummyCache : public ContainerBase<Config, DummyCache<Config>, false> {
    using base    = ContainerBase<Config, DummyCache<Config>, false>;
    using config  = Config;
    using key_t   = typename config::key_t;
    using value_t = typename config::value_t;

  public:
    DummyCache(size_t capacity, bool is_item_capacity) {
        allocateMemory(capacity, is_item_capacity);
    }

    void allocateMemory(size_t capacity, bool is_item_capacity) { this->init(capacity, is_item_capacity); }

    static const char* name() { return "Dummy"; }

    decltype(auto) profileStats() const { return profile_stats_.getSlice(); }

    size_t currentOverheadMemory() const { return 0; }

    static double elementSize() { return sizeof(lru_key_t) + sizeof(lru_value_t); }

    template <typename Producer, typename Consumer>
    void consumeCachedOrCompute(const key_t& key, const Producer& producer, Consumer& consumer) {
        consumer = producer();
    }

    void dump(){};

  private:
    static size_t memSizeForElements(size_t count) {
        return size_t(std::ceil(base::estimatedElementSize() * count));
    }

    typename config::profile_stats_t profile_stats_;
};
