#ifndef LRU_CACHE_H
#define LRU_CACHE_H

#include <cassert>
#include <mutex>

#include <boost/functional/hash.hpp>
#include <boost/optional.hpp>

#include "containers/container_base.h"
#include "utility.h"

/**
 * This class implements an LRU cache to store (Key, Value) tuple.
 * The cache is implemented with a (custom) hash_map datastructure. A
 * (Key,Value) pair will be stored according to its hash-value
 * computed by the Hasher object (using Hasher::operator() similarly
 * to boost::unordered_map). When the cache is full, some (Key,Value)
 * pair might be evicted from the cache and the Policy object will be
 * use to perform user defined clean-up by using
 * Policy::on_evict(Key&, Value&). When the LRUCache is destructed,
 * Policy::on_delete(Key&, Value&) will be called on each (Key,Value)
 * pair still in the cache. Key must have an equality operator defined
 * (Key::operator==).
 *
 * Implementation note: The LRUCache is implemented using a double
 * linked list for handling the LRU operations and choose which
 * element should be evicted. The hashmap is implemented with a bucket
 * data structure (an array of double linked list). To avoid memory
 * operations, all the memory needed by the cache are allocated in the
 * storage array. This implies that Key and Value must have a defaut
 * constructor and an affectation operator defined (Key::operator= and
 * Value::operator==).
 *
 * The bucket is a double linked list to allow an easy eviction. It
 * can be made a single linked list but that will require to call
 * Hasher::operator() and Key::operator== more often.
 *
 * The Hasher and the Policy are constructed using the default
 * constructor. But that can be hacked to construct them by copy.
 *
 * The cache can not be resized and the number of bucket is the
 * maximum number of element in the cache. There is no use of pointers
 * internally, but indexing in the storage array with 32-bit
 * integers. If a cache larger than 2**32 elements is required, the
 * data structure will need to be adapted (most like just changing the
 * type of index). If a cache smaller than 2**16 elements is useful,
 * the index could be changed to 16-bit integers to gain space.
 *
 * If the cache is believed to have bugs, turn on the DEBUG template
 * parameter and active assertions. That might not cache all the bugs,
 * but should catch some.
 **/

/**
 * For some reason the following code causes troubles [segmentation fault] on hopper where with a
current configuration _OPENMP is defined!!!
 *
#ifdef _OPENMP
template <typename Key, typename Value, typename Hasher = CombineHash, typename Policy =
no_eviction_policy<Key,Value>, typename Locking = openmp_locking, bool DEBUG=false , bool
PROFILE=false> #else template <typename Key, typename Value, typename Hasher = CombineHash, typename
Policy = no_eviction_policy<Key,Value>, typename Locking = no_locking, bool DEBUG=false , bool
PROFILE=false> #endif
**/

template <typename Config, unsigned int IgnoreBitsInHash = 0>
class LRUCache : public ContainerBase<Config, LRUCache<Config, IgnoreBitsInHash>, false> {
    using config  = Config;
    using key_t   = typename config::key_t;
    using value_t = typename config::value_t;
    using index_t = int;

    struct Element {
        index_t list_prev;   //-1 => head of list
        index_t list_next;   //-1 => tail of list
        index_t bucket_prev; //-x => head of bucket (x-1)
        index_t bucket_next; //-1 => tail of bucket
        key_t   key;
        value_t value;
    };

  public:
    LRUCache(size_t capacity = 0, bool is_item_capacity = false) {
        /// initialize a cache that stores size objects. Subsequently added object will cause an
        /// eviction.
        allocateMemory(capacity, is_item_capacity);
    }


    ~LRUCache() { releaseMemory(); }

    static const char* name() { return "LRU"; }

    decltype(auto) profileStats() const { return profile_stats_.getSlice(); }

    size_t currentOverheadMemory() const {
        return sizeof(index_t) * bucket_count_ +
               (sizeof(Element) - sizeof(key_t) - sizeof(value_t)) * this->current_element_count_;
    }

    static double elementSize() {
        return sizeof(Element) + sizeof(index_t) / config::hashTableLoadFactor();
    }

    void allocateMemory(size_t capacity, bool is_item_capacity) {
        this->init(capacity, is_item_capacity);
        if (capacity == 0) {
            return;
        }

        bucket_count_ = size_t(this->max_element_count_ / config::hashTableLoadFactor());
        storage_      = new Element[this->max_element_count_];

        // init "empty" linked list
        lru_list_head_ = -1;
        lru_list_tail_ = -1;

        for (index_t i = 0; i < this->max_element_count_ - 1; i++) {
            storage_[i].list_next = i + 1;
        }
        storage_[this->max_element_count_ - 1].list_next = -1;
        empty_nodes_head_                                = 0;

        bucket_ = new index_t[bucket_count_];
        for (int i = 0; i < bucket_count_; i++) {
            bucket_[i] = -1;
        }

        profile_stats_.reset();
        if (config::enable_debug) {
            assert(coherent());
        }
    }

    /// calls the eviction policy on all the objects in the cache
    void releaseMemory() {
        if (storage_) {
            //	call eviction policy
            for (index_t i = lru_list_head_; i != -1; i = storage_[i].list_next) {
                deletion_policy_.onDelete(storage_[i].key, storage_[i].value);
            }
        }
        //	free own memory
        delete[] bucket_;
        bucket_ = nullptr;
        delete[] storage_;
        storage_ = nullptr;
    }

    template <typename Producer, typename Consumer>
    void consumeCachedOrCompute(const key_t& key, const Producer& producer, Consumer& consumer) {
        if (find(key, consumer)) {
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
        return size_t(std::ceil(elementSize() * count));
    }

    /// might invalidate operator by evicting one object from the cache
    /// (eviction policy will be used)
    void insert(const key_t& k, const value_t& v) {
        typename config::lock_guard_t lg(lock_);
        profile_stats_.head_accesses++;

        if (config::enable_debug) {
            assert(coherent());
        }
        profile_stats_.insert++;

        while (this->current_element_count_ >= this->max_element_count_) {
            evict();
        }

        this->current_element_count_++;

        index_t newelem = empty_nodes_head_;
        if (config::enable_debug) {
            assert(newelem != -1);
        }
        // move current_element_count further
        empty_nodes_head_ = storage_[newelem].list_next;

        // affect values
        storage_[newelem].key   = k;
        storage_[newelem].value = v;

        // insert newelem at end of list
        storage_[newelem].list_prev = lru_list_tail_;
        storage_[newelem].list_next = -1;
        if (lru_list_tail_ != -1) {
            storage_[lru_list_tail_].list_next = newelem;
        }

        lru_list_tail_ = newelem;

        // if list is empty, tail is head
        if (lru_list_head_ == -1) {
            lru_list_head_ = lru_list_tail_;
        }

        // insert newelem in bucket at the head
        index_t wbuck = whichBucket(k);
        if (config::enable_debug) {
            assert(wbuck >= 0 && wbuck < bucket_count_);
        }

        storage_[newelem].bucket_next = bucket_[wbuck];
        if (bucket_[wbuck] != -1) {
            storage_[bucket_[wbuck]].bucket_prev = newelem;
        }
        storage_[newelem].bucket_prev = -wbuck - 1;
        bucket_[wbuck]                = newelem;

        if (config::enable_debug) {
            assert(coherent());
        }
    }

    template <typename Consumer>
    bool find(const key_t& k, Consumer& consumer) {
        typename config::lock_guard_t lg(lock_);
        profile_stats_.head_accesses++;

        index_t wbuck = whichBucket(k);
        if (config::enable_debug) {
            assert(wbuck >= 0 && wbuck < bucket_count_);
        }

        profile_stats_.find++;

        index_t current = bucket_[wbuck];

        if (config::enable_debug) {
            assert(coherent());
        }

        while (current != -1) {
            Element& current_elem = storage_[current];

            if (current_elem.key == k) {
                // update LRU list
                if (current != lru_list_tail_) { // no update to be done otherwise
                    // remove first
                    if (current_elem.list_prev == -1) { // at the beginning
                        lru_list_head_ = current_elem.list_next;
                    } else { // somewhere inside
                        storage_[current_elem.list_prev].list_next = current_elem.list_next;
                    }

                    storage_[current_elem.list_next].list_prev = current_elem.list_prev;

                    // then insert
                    current_elem.list_next             = -1;
                    storage_[lru_list_tail_].list_next = current;
                    current_elem.list_prev             = lru_list_tail_;
                    lru_list_tail_                     = current;

                    if (config::enable_debug) {
                        assert(coherent());
                    }
                }

                consumer = current_elem.value;
                return true;
            }

            current = current_elem.bucket_next;
        }

        // if I reach here, the object is not found
        return false;
    }

    /// chose which bucket a key is affected to.
    int whichBucket(const key_t& k) const { return (h_(k) >> IgnoreBitsInHash) % bucket_count_; }

    void evict() {
        if (config::enable_debug) {
            assert(coherent());
        }
        profile_stats_.evict++;
        index_t victim = lru_list_head_;
        if (config::enable_debug) {
            assert(victim >= 0);
        }
        // the successor of victim in list is the new head of list
        lru_list_head_                     = storage_[victim].list_next;
        storage_[lru_list_head_].list_prev = -1;

        // victim is the new head of empty_nodes_head
        storage_[victim].list_next = empty_nodes_head_;
        empty_nodes_head_          = victim;

        // remove victim from bucket
        if (storage_[victim].bucket_prev < 0) {
            bucket_[-storage_[victim].bucket_prev - 1] = storage_[victim].bucket_next;
        } else {
            storage_[storage_[victim].bucket_prev].bucket_next = storage_[victim].bucket_next;
        }

        if (storage_[victim].bucket_next >= 0) {
            storage_[storage_[victim].bucket_next].bucket_prev = storage_[victim].bucket_prev;
        }

        if (config::enable_debug) {
            assert(this->current_element_count_ >= 1);
        }
        this->current_element_count_--;

        // call eviction policy
        deletion_policy_.onDelete(storage_[victim].key, storage_[victim].value);

        if (config::enable_debug) {
            assert(coherent());
        }
    }

    /// returns false if an incoherency in the cache is detected. Notice
    /// that this function might not catch all inconsistencies.
    bool coherent() {
        {
            int nbcount = 0;
            for (index_t i = empty_nodes_head_; i != -1; i = storage_[i].list_next, nbcount++) {
                if (nbcount > this->max_element_count_) { // no loop
                    return false;
                }
            }
        }

        {
            int nbcount_for = 0;
            for (index_t i = lru_list_head_; i != -1; i = storage_[i].list_next, nbcount_for++) {
                if (nbcount_for > this->max_element_count_) { // no loop
                    return false;
                }
                if (storage_[i].bucket_prev < 0) { // head of bucket properly set (weaker test)
                    if (whichBucket(storage_[i].key) != -storage_[i].bucket_prev - 1) {
                        return false;
                    }
                }
            }

            int nbcount_back = 0;
            for (index_t i = lru_list_tail_; i != -1; i = storage_[i].list_prev, nbcount_back++) {
                if (nbcount_back > this->max_element_count_) { // no loop
                    return false;
                }
            }

            if (nbcount_back != nbcount_for) { // as many element in forward list and reverse list
                return false;
            }
        }

        {
            for (int j = 0; j < bucket_count_; j++) {
                index_t i;
                int     nbcount = 0;
                for (i = bucket_[j]; i >= 0; nbcount++, i = storage_[i].bucket_next) {
                    if (i == this->current_element_count_) {
                        // an element in a bucket is not in the empty list (test weaker)
                        return false;
                    }
                    if (j != whichBucket(storage_[i].key)) { // is the element in the right bucket
                        return false;
                    }
                    if (storage_[i].bucket_next == storage_[i].bucket_prev &&
                        storage_[i].bucket_prev >= 0) { // no cycle (test weaker)
                        return false;
                    }
                    if (nbcount > this->max_element_count_) { // no loop
                        return false;
                    }
                }
            }
        }

        return true;
    }

    // We have to use composition to preserve late initialization semantic
    Element* storage_;
    index_t* bucket_; // give head of bucket
    index_t  lru_list_head_;
    index_t  lru_list_tail_;
    index_t  empty_nodes_head_;

    size_t bucket_count_;

    typename config::hasher_t        h_;
    typename config::deletion_policy deletion_policy_;
    typename config::locking_t       lock_;
    typename config::profile_stats_t profile_stats_;
};

template <typename Config, unsigned int IgnoreBitsInHash>
void LRUCache<Config, IgnoreBitsInHash>::dump() {
    std::cout << "--- raw ---" << std::endl;
    std::cout << "list_head:" << lru_list_head_ << " list_tail: " << lru_list_tail_ << std::endl;
    std::cout << "current_element_count: " << this->current_element_count_ << std::endl;

    std::cout << "storage" << std::endl;
    for (index_t i = 0; i < this->current_element_count_; i++) {
        Element& e = storage_[i];
        std::cout << i << " : " << e.list_prev << " " << e.list_next << " " << e.bucket_prev << " "
                  << e.bucket_next << " " << e.key << " " << e.value << std::endl;
    }

    std::cout << "bucket" << std::endl;
    for (int i = 0; i < bucket_count_; i++) {
        std::cout << i << " : " << bucket_[i] << std::endl;
    }
    std::cout << "--- pretty ---" << std::endl;
    std::cout << "empty list" << std::endl;
    for (index_t i = this->current_element_count_; i != -1; i = storage_[i].list_next)
        std::cout << i << " ";
    std::cout << std::endl;
    std::cout << "LRU list" << std::endl;
    for (index_t i = lru_list_head_; i != -1; i = storage_[i].list_next)
        std::cout << i << " ";
    std::cout << std::endl;
    std::cout << "LRU list (in reverse)" << std::endl;
    for (index_t i = lru_list_tail_; i != -1; i = storage_[i].list_prev)
        std::cout << i << " ";
    std::cout << std::endl;
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
        std::cout << "// ";
        for (; i >= 0; i = storage_[i].bucket_prev)
            std::cout << i << " ";
        std::cout << std::endl;
    }
}

#endif
