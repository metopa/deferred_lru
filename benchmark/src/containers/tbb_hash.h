#pragma once
#include <cassert>

#include "config.h"
#include "container_base.h"
#include "utility.h"

#include <tbb/concurrent_hash_map.h>

#include <chrono>
#include <mutex>

template <typename Config>
class TbbHash : public ContainerBase<Config, TbbHash<Config>, true> {
    using index_t = int;

    using base      = ContainerBase<Config, TbbHash<Config>, true>;
    using config    = Config;
    using key_t     = typename config::key_t;
    using value_t   = typename config::value_t;
    using storage_t = tbb::concurrent_hash_map<key_t, value_t>;

    static constexpr decltype(auto) load_factor = config::hashTableLoadFactor();

    struct Element {
        std::atomic<index_t> bucket_next; //-1 => tail of bucket
        key_t                key;
        value_t              value;
    };

  public:
    /// initializes a TbbHash that stores size objects. Subsequently added object will cause an
    /// undefined behavior
    TbbHash(size_t capacity, bool is_item_capacity) { allocateMemory(capacity, is_item_capacity); }

    ~TbbHash() { releaseMemory(); }

    static const char* name() { return "TbbHash"; }

    decltype(auto) profileStats() const { return profile_stats_.getSlice(); }

    size_t currentOverheadMemory() const {
        return this->current_element_count_ * (sizeof(Element) - sizeof(key_t) - sizeof(value_t));
    }

    static double elementSize() { return sizeof(Element) + sizeof(index_t) / load_factor; }

    void allocateMemory(size_t capacity, bool is_item_capacity) {
        this->init(capacity, is_item_capacity);

        storage_.clear();

        profile_stats_.reset();
    }

    /// calls the policy on all the objects in the cache
    void releaseMemory() { storage_.clear(); }

    template <typename Producer>
    value_t getCachedOrCompute(const key_t& key, const Producer& producer) {
        value_t value;
        consumeCachedOrCompute(key, producer, value);
        return value;
    }

    template <typename Producer, typename Consumer>
    void consumeCachedOrCompute(const key_t& key, const Producer& producer, Consumer& consumer) {
        {
            profile_stats_.find++;
            typename storage_t::const_accessor ac;
            if (storage_.find(ac, key)) {
                consumer = ac->second;
                ac.release();
                return;
            }
        }

        profile_stats_.insert++;
        auto x   = producer();
        consumer = x;
        typename storage_t::accessor ac{};
        if (storage_.insert(ac, key)) {
            ac->second = x;
            ac.release();
        }
    }

  private:
    static size_t memSizeForElements(size_t count) {
        return size_t(std::ceil(base::estimatedElementSize() * count));
    }

    storage_t                        storage_;
    typename config::profile_stats_t profile_stats_;
};
