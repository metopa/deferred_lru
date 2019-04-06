#pragma once

#include <boost/functional/hash.hpp>

#include <mutex>

#include "utility.h"

struct TrivialHash {
    template <typename Int>
    inline size_t operator()(Int i) const {
        return size_t(i);
    }
};

struct EmptyDeletePolicy {
    template <typename KeyT, typename ValueT>
    void onDelete(const KeyT&, const ValueT&) {}
};

/**
 * This structure provides all necessary type arguments for a lookup container.
 * Type arguments should provide the following operations
 *
 * @tparam KeyT
 *          - bool operator==(KeyT)
 *          - bool operator<(KeyT)
 *
 * @tparam ValueT
 *
 * @tparam HasherT
 *          - size_t operator()(KeyT)
 *
 * @tparam LockingT
 *          - void lock()
 *          - void unlock()
 *          - bool try_lock()
 *
 * @tparam DeletionPolicy
 *          - void on_delete(KeyT, ValueT)
 *             // This function can perform some actions
 *             // not defined in item destructor
 *
 * @tparam EnableDebug
 *          // Enable additional runtime checks
 *
 * @tparam EnableProfile
 *          // Enable additional logging
 */

template <typename KeyT, typename ValueT, typename HasherT, typename CompT, typename LockingT,
          typename DeletionPolicy = EmptyDeletePolicy, int HashTableLoadFactor = 4,
          bool EnableDebug = false, bool EnableProfile = false>
struct ContainerConfig {
    using key_t            = KeyT;
    using value_t          = ValueT;
    using hasher_t         = HasherT;
    using comparator_t     = CompT;
    using locking_t        = LockingT;
    using lock_guard_t     = std::lock_guard<locking_t>;
    using deletion_policy  = DeletionPolicy;
    using idx_t            = size_t;
    using metric_counter_t = std::conditional_t<false, MetricCounterImpl, MetricCounterStub>;
    using profile_stats_t  = ProfileStats<EnableProfile>;

    enum { enable_debug = EnableDebug, enable_profile = EnableProfile };

    static constexpr double hashTableLoadFactor() { return HashTableLoadFactor; }
};
