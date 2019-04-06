#pragma once

#include <memory>

#include "containers/concurrent_lru.h"
#include "deferred_lru.h"
#include "containers/lru.h"

template <typename Config, typename ContainerT, unsigned LogBucketCount>
class BucketedAdapter {
    using key_t   = typename Config::key_t;
    using value_t = typename Config::value_t;

  public:
    BucketedAdapter() { containers_.reset(new ContainerT[bucketCount()]); }

    BucketedAdapter(size_t capacity, bool is_item_capacity) : BucketedAdapter() {
        allocateMemory(capacity, is_item_capacity);
    }

    static constexpr size_t bucketCount() { return 1u << LogBucketCount; }

    void allocateMemory(size_t capacity, bool is_item_capacity) {
        for (size_t i = 0; i < bucketCount(); i++) {
            containers_[i].allocateMemory(capacity / bucketCount() +
                                              (i == 0 ? capacity % bucketCount() : 0),
                                          is_item_capacity);
        }
    }

    /// calls the policy on all the objects in the cache
    void releaseMemory() {
        for (size_t i = 0; i < bucketCount(); i++) {
            containers_[i].releaseMemory();
        }
    }

    static const char* name() {
        static std::string s = std::string("Bucket<") + ContainerT::name() + ", " +
                               std::to_string(bucketCount()) + ">";
        return s.c_str();
    }

    decltype(auto) profileStats() const {
        auto res = containers_[0].profileStats();
        for (size_t i = 1; i < bucketCount(); i++)
            res += containers_[i].profileStats();
        return res;
    }

    template <typename Producer, typename Consumer>
    void consumeCachedOrCompute(const key_t& key, const Producer& producer, Consumer& consumer) {
        size_t bucket_nr = getBucketNr(hasher_(key));
        containers_[bucket_nr].consumeCachedOrCompute(key, producer, consumer);
    }

    size_t getBucketNr(size_t hash) {
        std::array<uint16_t, sizeof(hash) / 2> d;
        *reinterpret_cast<size_t*>(d.data()) = hash;
        for (int i = 1; i < d.size(); i++)
            d[0] ^= d[i];
        const size_t mask = bucketCount() - 1;
        return d[0] & mask;
    }

    MemStats memStats() const {
        auto res = containers_[0].memStats();
        for (size_t i = 1; i < bucketCount(); i++) {
            res += containers_[i].memStats();
        }

        return res;
    }

    size_t currentOverheadMemory() const {
        auto res = containers_[0].currentOverheadMemory();
        for (size_t i = 1; i < bucketCount(); i++) {
            res += containers_[i].currentOverheadMemory();
        }

        return res;
    }

  private:
    typename Config::hasher_t     hasher_;
    std::unique_ptr<ContainerT[]> containers_;
};

template <typename Config>
class BucketedLRU : public BucketedAdapter<Config, LRUCache<Config>, 6> {
    using BucketedAdapter<Config, LRUCache<Config>, 6>::BucketedAdapter;
};

template <typename Config>
class BucketedDeferredLRU : public BucketedAdapter<Config, DeferredLRU<Config>, 6> {
    using BucketedAdapter<Config, DeferredLRU<Config>, 6>::BucketedAdapter;
};

template <typename Config>
class BucketedConcurrentLRU : public BucketedAdapter<Config, ConcurrentLRU<Config>, 6> {
    using BucketedAdapter<Config, ConcurrentLRU<Config>, 6>::BucketedAdapter;
};
