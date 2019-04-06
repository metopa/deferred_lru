#ifndef HASH_FIXED_H
#define HASH_FIXED_H

#include <cassert>

#include "config.h"
#include "container_base.h"
#include "utility.h"

#include <chrono>
#include <mutex>

/**
 * This class implements a HashMap of fixed size to store (Key, Value)
 * tuples. A (Key,Value) pair will be stored according to its
 * hash-value computed by the Hasher object (using Hasher::operator()
 * similarly to boost::unordered_map). When the HashFixed is full,
 * subsequent insertions lead to an undefined behavior (most likely
 * segmentation fault). When the HasFixed is destructed,
 * Policy::on_delete(Key&, Value&) will be called on each (Key,Value)
 * pair that has been inserted. Key must have an equality operator
 * defined (Key::operator==).
 *
 * Implementation note: The hashmap is implemented with a bucket data
 * structure (an array of single linked list). To avoid memory
 * operations, all the memory needed is allocated in the storage
 * array. This implies that Key and Value must have a defaut
 * constructor and an affectation operator defined (Key(),
 * Key::operator=(), Value() and Value::operator=()), also Keys needs
 * to be comparable (Key must define Key::operator==()) NOTE: this
 * could be fixed using placement new but did not seem useful at
 * first.
 *
 * The Hasher and the Policy are constructed using the default
 * constructor. But that can be hacked to construct them by copy.
 *
 * The HashFixed can not be resized and the number of bucket is the
 * maximum number of element in the HashMap. There is no use of
 * pointers internally, but indexing in the storage array with 32-bit
 * integers. If a HashFixed larger than 2**32 elements is required,
 * the data structure will need to be adapted (most likely just
 * changing the type of index). If a HashFixed smaller than 2**16
 * elements is useful, the index could be changed to 16-bit integers
 * to gain space.
 *
 * If the HashFixed is believed to have bugs, turn on the DEBUG
 * template parameter and activate assertions. That might not detect all
 * the bugs, but should catch some. In particular DEBUG verifies how
 * many elements are added in the HasFixed.
 **/

/**
 * I have no idea why the following code does not work properly on hopper. It causes segmentation
fault. _OPENMP seems to be defined
 *
#ifdef _OPENMP
template <typename Key, typename Value, typename Hasher, typename DeletePolicy =
NoDeletePolicy<Key,Value>, typename Locking = openmp_locking, bool DEBUG=false , bool PROFILE=false>
#else
template <typename Key, typename Value, typename Hasher, typename DeletePolicy =
NoDeletePolicy<Key,Value>, typename Locking = no_locking, bool DEBUG=false , bool PROFILE=false>
#endif
**/
template <typename Config>
class HashFixed : public ContainerBase<Config, HashFixed<Config>, true> {
    // TODO Unsigned index
    // TODO null value
    // TODO Rename iterators
    using index_t = int;

    using base    = ContainerBase<Config, HashFixed<Config>, true>;
    using config  = Config;
    using key_t   = typename config::key_t;
    using value_t = typename config::value_t;

    static constexpr decltype(auto) load_factor = config::hashTableLoadFactor();

    struct Element {
        std::atomic<index_t> bucket_next; //-1 => tail of bucket
        key_t   key;
        value_t value;
    };

  public:
    /// initializes a HashFixed that stores size objects. Subsequently added object will cause an
    /// undefined behavior
    HashFixed(size_t capacity, bool is_item_capacity) {
        allocateMemory(capacity, is_item_capacity);
    }

    ~HashFixed() { releaseMemory(); }

    static const char* name() { return "HashFixed2"; }

    decltype(auto) profileStats() const { return profile_stats_.getSlice(); }

    size_t currentOverheadMemory() const {
        return sizeof(index_t) * bucket_count_ +
               this->current_element_count_ * (sizeof(Element) - sizeof(key_t) - sizeof(value_t));
    }

    static double elementSize() { return sizeof(Element) + sizeof(index_t) / load_factor; }

    void allocateMemory(size_t capacity, bool is_item_capacity) {
        this->init(capacity, is_item_capacity);

        bucket_count_ = size_t(this->max_element_count_ / load_factor);

        storage_ = new Element[this->max_element_count_];
        bucket_  = new std::atomic<index_t>[bucket_count_];
        for (size_t i = 0; i < bucket_count_; i++) {
            bucket_[i] = -1;
        }

        profile_stats_.reset();
    }

    /// calls the policy on all the objects in the cache
    void releaseMemory() {
        if (storage_) {
            //	call eviction policy
            for (index_t i = 0; i != this->current_element_count_; i++) {
                deletion_policy_.onDelete(storage_[i].key, storage_[i].value);
            }
        }
        //	free own memory
        delete[] bucket_;
        delete[] storage_;
        bucket_  = nullptr;
        storage_ = nullptr;
    }

    template <typename Producer>
    value_t getCachedOrCompute(const key_t& key, const Producer& producer) {
        value_t value;
        consumeCachedOrCompute(key, producer, value);
        return value;
    }

    template <typename Producer, typename Consumer>
    void consumeCachedOrCompute(const key_t& key, const Producer& producer, Consumer& consumer) {
        auto it = find(key);
        if (it) {
            consumer = it->value;
            return;
        }

        auto x   = producer();
        consumer = x;
        insert(key, x);
    }

    /// Calling this function outputs the internal structure of the
    /// cache to stdout. Useful for debugging only.
    void dump();

  private:
    static size_t memSizeForElements(size_t count) {
        return size_t(std::ceil(base::estimatedElementSize() * count));
    }

    /// never invalidate iterators
    void insert(const key_t& k, const value_t& v) {
        if (this->current_element_count_ >= this->max_element_count_) {
            profile_stats_.head_accesses++;
            return;
        }

        profile_stats_.insert++;

        index_t newelem = static_cast<index_t>(this->current_element_count_++);
        if (newelem >= this->max_element_count_) {
            profile_stats_.head_accesses++;
            this->current_element_count_--;
            return;
        }

        // affect values
        storage_[newelem].key   = k;
        storage_[newelem].value = v;

        // insert newelem in bucket at the head
        index_t wbuck = whichBucket(k);
        index_t next = bucket_[wbuck].load();
        do {
            storage_[newelem].bucket_next.store(next, std::memory_order_relaxed);
        } while (!bucket_[wbuck].compare_exchange_weak(next, newelem));
    }

    Element* find(const key_t& k) {
        index_t wbuck = whichBucket(k);

        profile_stats_.find++;

        index_t current = bucket_[wbuck].load();

        while (current != -1) {
            Element& current_elem = storage_[current];

            if (current_elem.key == k) {
                return &current_elem;
            }

            current = current_elem.bucket_next.load(std::memory_order_acquire);
        }

        // if I reach here, the object is not found
        return nullptr;
    }

    /// chose which bucket a key is affected to.
    int whichBucket(const key_t& k) const { return hasher_(k) % bucket_count_; }

    std::atomic<index_t>* bucket_; // give head of bucket
    Element* storage_;

    size_t bucket_count_;

    typename config::hasher_t        hasher_;
    typename config::deletion_policy deletion_policy_;
    typename config::profile_stats_t profile_stats_;
};

template <typename Config>
void HashFixed<Config>::dump() {
    std::cout << "--- raw ---" << std::endl;
    std::cout << "current_element_count: " << this->current_element_count_ << std::endl;

    std::cout << "storage" << std::endl;
    for (index_t i = 0; i < this->max_element_count_; i++) {
        Element& e = storage_[i];
        std::cout << i << " : " << e.bucket_next << " " << e.key << " " << e.value << std::endl;
    }

    std::cout << "bucket" << std::endl;
    for (int i = 0; i < bucket_count_; i++) {
        std::cout << i << " : " << bucket_[i] << std::endl;
    }
    std::cout << "--- pretty ---" << std::endl;
    std::cout << "buckets" << std::endl;
    for (int j = 0; j < bucket_count_; j++) {
        index_t i;
        index_t old_i   = -1;
        int     nbcount = 0;
        for (i     = bucket_[j]; i >= 0 && nbcount < this->max_element_count_;
             old_i = i, i = storage_[i].bucket_next, nbcount++) {
            assert(old_i != i);
            std::cout << i << " ";
        }
        i = old_i;
        std::cout << std::endl;
    }
}

#endif
