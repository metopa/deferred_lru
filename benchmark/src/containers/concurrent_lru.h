#pragma once

#include <cassert>
#include <csignal>
#include <mutex>
#include <set>
#include <vector>

#include <boost/functional/hash.hpp>
#include <boost/optional.hpp>

#include <folly/PackedSyncPtr.h>

#include "containers/container_base.h"
#include "containers/lru.h"
#include "utility.h"

#define TRACE_LOCKS 0
#define TRACE_HT 0
#define DISABLE_LOCKS 0
#define HT_EXTERNAL_LOCK 1

template <typename Config, unsigned int IgnoreBitsInHash = 0>
class ConcurrentLRU : public ContainerBase<Config, ConcurrentLRU<Config, IgnoreBitsInHash>, false> {
    using config  = Config;
    using key_t   = typename config::key_t;
    using value_t = typename config::value_t;
    template <typename T>
    using sync_ptr_t = folly::PackedSyncPtr<T>;
    using backoff_t  = folly::detail::Sleeper;
    using lock_t     = typename config::locking_t;

    class NodeBase {
        sync_ptr_t<NodeBase> sync_ptr_;

      public:
        NodeBase() { sync_ptr_.init(0); }

        NodeBase* htNext() { return sync_ptr_.get(); }

        void htSetNext(NodeBase* next) { sync_ptr_.set(next); }

        void lock() {
#if !DISABLE_LOCKS
            sync_ptr_.lock();
#endif
        }

        bool try_lock() {
#if !DISABLE_LOCKS
            return sync_ptr_.try_lock();
#else
            return true;
#endif
        }

        void unlock() {
#if !DISABLE_LOCKS
            sync_ptr_.unlock();
#endif
        }

        bool isLocked() { return false; }

        void lruSetFlag(bool set) {
            auto flags = sync_ptr_.extra();
            flags &= ~1u;
            flags |= set * 1;
            sync_ptr_.setExtra(flags);
        }

        bool lruFlag() { return (bool)(sync_ptr_.extra() & 1); }

        void htSetFlag(bool set) {
            auto flags = sync_ptr_.extra();
            flags &= ~2u;
            flags |= set * 2;
            sync_ptr_.setExtra(flags);
        }

        bool htFlag() { return (bool)(sync_ptr_.extra() & 2); }
    };

    class Node : public NodeBase {
      public:
        Node() : lru_next(nullptr), lru_prev(nullptr), key(), value() {}

        bool dataIsValidForKey(const key_t& target_key) {
            return this->lruFlag() && target_key == key;
        }

        Node* lru_next;
        Node* lru_prev;

        key_t   key;
        value_t value;
    };

  public:
    explicit ConcurrentLRU(size_t capacity = 0, bool is_item_capacity = false) {
        allocateMemory(capacity, is_item_capacity);
        #if TRACE_LOCKS
        std::cout << "pool_head: " << &pool_head_ << "\n";
        std::cout << "pool_tail: " << &pool_tail_ << "\n";
        std::cout << "lru_head:  " << &lru_head_ << "\n";
        std::cout << "lru_tail:  " << &lru_tail_ << "\n";
        #endif
    }

    ~ConcurrentLRU() { releaseMemory(); }

    static const char* name() { return "ConcurrentLRU"; }

    decltype(auto) profileStats() const { return profile_stats_.getSlice(); }

    size_t memoryUsage() const { return this->capacity(); }

    size_t currentOverheadMemory() const {
        return sizeof(sync_ptr_t<Node>) * ht_.size() +
               (sizeof(Node) - sizeof(key_t) - sizeof(value_t)) * this->current_element_count_;
    }

    static double elementSize() {
        return sizeof(Node) + sizeof(sync_ptr_t<Node>) / (double)config::hashTableLoadFactor();
    }

    void allocateMemory(size_t capacity, bool is_item_capacity) {
        this->init(capacity, is_item_capacity);
        if (capacity == 0) {
            return;
        }

        ht_.assign(size_t((this->max_element_count_ + config::hashTableLoadFactor() - 1) /
                          config::hashTableLoadFactor()),
                   NodeBase());
        ht_locks_.reset(new lock_t[ht_.size()]);
        data_.resize(this->max_element_count_);

#if TRACE_LOCKS
        std::cout << "data:      " << data_.data() << "\n           " << data_.data() + data_.size()
                  << "\n";
        std::cout << "size:      " << data_.size() << std::endl;
#endif

        // init "empty" linked list
        lru_head_.lru_next = &lru_tail_;
        lru_tail_.lru_prev = &lru_head_;
        lru_head_.lruSetFlag(true);
        lru_head_.htSetFlag(true);
        lru_tail_.lruSetFlag(true);
        lru_tail_.htSetFlag(true);

        auto prev = &pool_head_;
        for (auto& node : data_) {
            prev->lru_next = &node;
            node.lru_prev  = prev;
            prev           = &node;
        }

        data_.back().lru_next = &pool_tail_;
        pool_tail_.lru_prev   = &data_.back();

        profile_stats_.reset();
    }

    /// calls the eviction policy on all the objects in the cache
    void releaseMemory() {
        for (auto& bucket : ht_) {
            Node* node = reinterpret_cast<Node*>(bucket.htNext());
            while (node) {
                deleter_.onDelete(std::move(node->key), std::move(node->value));
                node = reinterpret_cast<Node*>(node->htNext());
            }
        }
        data_.clear();
        data_.shrink_to_fit();
        ht_.clear();
        ht_.shrink_to_fit();
        ht_locks_.reset();
    }

    bool insert(const key_t& key, value_t value) {
        profile_stats_.insert++;

        // Reserving space for the element beforehand
        Node* node = acquireFreeNode();

        // node is locked
        node->key   = std::move(key);
        node->value = std::move(value);
        this->current_element_count_++;
        node->lruSetFlag(true);
        lruInsertLast(node);

        if (!htInsert(node)) {
            bool evicted_from_lru = lruRemoveNode(node);

            assert(evicted_from_lru);
            node->lruSetFlag(false);

            // node is locked
            deleter_.onDelete(std::move(node->key), std::move(node->value));
            this->current_element_count_--;
            putNodeToPool(node);
            _unlockNode(node, "api.insert:fail");

            return false;
        }
        _unlockNode(node, "api.insert:success");

        return true;
    }

    template <typename Consumer>
    bool find(const key_t& key, Consumer& consumer) {
        profile_stats_.find++;
        Node* node = htFind(key);

        if (!node) {
            return false;
        }

        _lockNode(node, "api.find");
        if (node->dataIsValidForKey(key)) {
            consumer = node->value;
            lruMoveToTail(node);
            _unlockNode(node, "api.find:ok");
            return true;
        } else {
            _unlockNode(node, "api.find:fail");
            return false;
        }
    }

    template <typename Producer, typename Consumer>
    void consumeCachedOrCompute(const key_t& key, const Producer& producer, Consumer& consumer) {
        if (find(key, consumer)) {
            return;
        }

        auto x   = producer();
        consumer = x;
        insert(key, std::move(x));
    }

    void assertIsCoherent();

    void dump();

  private:
    static size_t memSizeForElements(size_t count) {
        return size_t(std::ceil(elementSize() * count));
    }

    void dumpList(Node* node);

    std::string dumpNode(Node* node);

    void dumpHt();

    void dumpNodeExt(char* buf, const Node* node, bool print_addr = true) const;

    void _lockNode(Node* node, const char* reason) {
#if TRACE_LOCKS
        char name[100], buf[100];
        dumpNodeExt(name, node);
        sprintf(buf, "%d: ?%s[%s]\n", omp_get_thread_num(), reason, name);
        std::cerr << buf;
#endif

        node->lock();

#if TRACE_LOCKS
        sprintf(buf, "%d: +%s[%s]\n", omp_get_thread_num(), reason, name);
        std::cerr << buf;
#endif
    }

    void _unlockNode(Node* node, const char* reason) {
#if TRACE_LOCKS
        char name[100], buf[100];
        dumpNodeExt(name, node);
        sprintf(buf, "%d: -%s[%s]\n", omp_get_thread_num(), reason, name);
        std::cerr << buf;
#endif

        node->unlock();
    }

    bool _tryLockNode(Node* node, const char* reason) {
        bool success = node->try_lock();

#if TRACE_LOCKS
        char name[100], buf[100];
        dumpNodeExt(name, node);
        sprintf(buf, "%d: @%c%s[%s]\n", omp_get_thread_num(), success ? '+' : '-', reason, name);
        std::cerr << buf;
#endif

        return success;
    }

    // Evict nodes while dynamic memory exceeds
    // Get last node or get node from pool
    // Returned node must be locked
    Node* acquireFreeNode() {
        Node* node = nullptr;
        // If we go under limit at least once,
        // it means that we (with possible help of other threads)
        // freed at least used_dynamic_mem_

        while (node == nullptr) {
            node = getNodeFromPool();
            if (node == nullptr) {
                node = evictLeastRecentlyUsed();
            }
        }

        return node;
    }

    /**
     * return:
     *   unlocked->locked
     */
    Node* listEvictNext(Node* prev, Node* last_node) {
        _lockNode(prev, "evict.prev");
        Node* node = prev->lru_next;
        if (node == last_node) {
            _unlockNode(prev, "evict.prev:empty");
            return nullptr;
        }
        _lockNode(node, "evict.node");
        Node* next = node->lru_next;

        _lockNode(next, "evict.next");

        prev->lru_next = next;
        next->lru_prev = prev;
        _unlockNode(prev, "evict.prev");
        _unlockNode(next, "evict.next");

        node->lru_next = node->lru_prev = nullptr;
        return node;
    }

    /**
     * node:
     *   unlocked->unlocked
     *   must not be removed from the list
     *   must not be head
     *
     * new_prev:
     *   must not be in any list
     *   locked->locked
     */
    void listInsertBefore(Node* node, Node* new_prev) {
        while (true) {
            Node* prev = node->lru_prev;
            assert(prev != nullptr);
            _lockNode(prev, "insert.prev");
            if (prev->lru_next != node) {
                if (Config::enable_debug) {
                    if (prev == node->lru_prev) {
                        std::cerr << "Whoa: new prev: " << dumpNode(new_prev) << '\n';
                        std::cerr << "\t" << dumpNode(prev) << "->" << dumpNode(prev->lru_next)
                                  << std::endl;
                        std::cerr << "\t" << dumpNode(node->lru_prev) << "<-" << dumpNode(node)
                                  << std::endl;
                        std::raise(SIGINT);
                    }
                }
                _unlockNode(prev, "insert.prev:next_changed");
                continue;
            }

            _lockNode(node, "insert.next");

            assert(prev->lru_next == node);
            assert(prev == node->lru_prev);

            prev->lru_next     = new_prev;
            new_prev->lru_prev = prev;
            new_prev->lru_next = node;
            node->lru_prev     = new_prev;

            _unlockNode(prev, "insert.prev");
            _unlockNode(node, "insert.next");

            break;
        }
    }

    /**
     * node:
     *   locked->locked
     */
    void putNodeToPool(Node* node) { listInsertBefore(&pool_tail_, node); }

    /**
     *
     * return:
     *   unlocked->locked
     */
    Node* getNodeFromPool() { return listEvictNext(&pool_head_, &pool_tail_); }

    /**
     * node:
     *   locked->locked
     */
    void lruInsertLast(Node* node) {
        profile_stats_.head_accesses++;
        listInsertBefore(&lru_tail_, node);
    }

    /**
     * return:
     *   unlocked->locked
     */
    Node* lruEvictHead() { return listEvictNext(&lru_head_, &lru_tail_); }

    /**
     * return:
     *   locked->locked
     *   lru flag: + -> ?
     */
    void lruMoveToTail(Node* node) {
        assert(node->lruFlag());

        if (lruRemoveNode(node)) {
            lruInsertLast(node);
        }
    }

    /**
     * return:
     *   locked -> locked
     *   lru flag: + -> ?
     *
     * TODO: check if it worth make unlocked->locked
     */
    bool lruRemoveNode(Node* node) {
        assert(node->lruFlag());
        Node* prev;

        // lock parent
        while (true) {
            prev = node->lru_prev;
            assert(prev != nullptr);

            if (_tryLockNode(prev, "lru.remove.prev")) {
                break;
            } else {
                _unlockNode(node, "lru.remove.node:prev_locked");
                _lockNode(prev, "lru.remove.prev");

                if (prev->lru_next == node) {
                    // prev haven't changed
                    _lockNode(node, "lru.remove.node:prev_same");

                    if (!node->lruFlag()) {
                        // both prev and not are not in LRU anymore
                        _unlockNode(prev, "lru.remove.prev:not_lru_flag");
                        return false;
                    } else {
                        // both prev and node locked and in LRU
                        break;
                    }
                } else {
                    _unlockNode(prev, "lru.remove.prev:next_changed");

                    _lockNode(node, "lru.remove.node:relock");
                    // node could be removed from lru list now
                    if (!node->lruFlag()) {
                        // other thread has already removed it from LRU
                        return false;
                    } else {
                        // start again; node locked, prev is unlocked
                        continue;
                    }
                }
            }
        }

        Node* next = node->lru_next;

        if (Config::enable_debug) {
            assert(next != nullptr);
            if (prev == node || prev == next || node == next) {
                // used to catch one sneaky bug
                std::cerr << "Whoa:\n\t" << dumpNode(prev) << "::" << dumpNode(node)
                          << "::" << dumpNode(next) << '\n';
                std::cerr << "\t" << dumpNode(prev) << "->" << dumpNode(prev->lru_next) << "->"
                          << dumpNode(prev->lru_next->lru_next) << std::endl;
                std::cerr << "\t" << dumpNode(next->lru_prev->lru_prev) << "<-"
                          << dumpNode(next->lru_prev) << "<-" << dumpNode(next) << std::endl;
                std::raise(SIGINT);
            }
        }

        _lockNode(next, "lru.remove.next");

        prev->lru_next = next;
        next->lru_prev = prev;
        _unlockNode(prev, "lru.remove.prev");
        _unlockNode(next, "lru.remove.next");

        node->lru_next = node->lru_prev = nullptr;

        return true;
    }

    /**
     * return:
     *   locked
     */
    Node* evictLeastRecentlyUsed() {
        Node* node = lruEvictHead();
        if (!node) {
            return nullptr;
        }

        node->lruSetFlag(false);
        profile_stats_.evict++;
        backoff_t backoff1;
        while (!node->htFlag()) {
            backoff1.wait();
        }
        backoff_t backoff2;
        while (!htRemove(node)) {
            backoff2.wait();
        }
        deleter_.onDelete(std::move(node->key), std::move(node->value));

        this->current_element_count_--;
        return node;
    }

    size_t whichBucket(const key_t& k) const {
        return (hasher_(k) >> IgnoreBitsInHash) % ht_.size();
    }

    void lockBucket(size_t bucket_nr) {
        if (HT_EXTERNAL_LOCK) {
            ht_locks_[bucket_nr].lock();
        } else {
            ht_[bucket_nr].lock();
        }
    }

    void unlockBucket(size_t bucket_nr) {
        if (HT_EXTERNAL_LOCK) {
            ht_locks_[bucket_nr].unlock();
        } else {
            ht_[bucket_nr].unlock();
        }
    }

    // insert to head
    // TODO: check no such node with a key?
    // bucket must be locked during operation
    // bucket must be unlocked in the end
    // node must be locked
    bool htInsert(Node* node) {
        auto bucket_nr = whichBucket(node->key);
        lockBucket(bucket_nr);

#if TRACE_HT
        char buf1[100], buf2[100];
        dumpNodeExt(buf1, node, false);
        dumpNodeExt(buf2, (Node*)ht_[bucket_nr].htNext(), false);
        char msg[300];
        std::sprintf(msg, "%d: <%zd>: +%s => <%zd>::%s::%s\n", omp_get_thread_num(), bucket_nr,
                     buf1, bucket_nr, buf1, buf2);
        std::cerr << msg;
#endif

        node->htSetNext(ht_[bucket_nr].htNext());
        ht_[bucket_nr].htSetNext(node);
        node->htSetFlag(true);
        unlockBucket(bucket_nr);
        return true;
    }

    // Lock bucket and search node
    // TODO: check node locking
    // return:
    //   unlocked->unlocked
    Node* htFind(const key_t& key) {
        auto bucket_nr = whichBucket(key);

#if TRACE_HT
        char                 name[100];
        volatile const char* prevent_opt;
        volatile bool        dump = false;
#endif

        lockBucket(bucket_nr);
        Node* node = reinterpret_cast<Node*>(ht_[bucket_nr].htNext());

        while (node) {
#if TRACE_HT
            dumpNodeExt(name, node);
            prevent_opt = name;
            if (dump) {
                dumpHt();
                assertIsCoherent();
            }
#endif
            if (node->key == key) {
                break;
            }
            node = reinterpret_cast<Node*>(node->htNext());
        }

        unlockBucket(bucket_nr);
        return node;
    }

    // Lock bucket and remove node
    // Node is locked
    // fails if prev node is locked (it may be being simultaneously evicted by another thread)
    bool htRemove(Node* node) {
        auto bucket_nr = whichBucket(node->key);
        lockBucket(bucket_nr);

        NodeBase* prev    = &ht_[bucket_nr];
        NodeBase* current = prev->htNext();

        while (current) {
            if (current == node) {
                if (prev != &ht_[bucket_nr]) {
                    if (!_tryLockNode((Node*)prev, "ht.remove.prev")) {
                        // prev is already locked, probably being removed
                        unlockBucket(bucket_nr);
                        return false;
                    }
                }

                auto next = node->htNext();
                prev->htSetNext(next);
                if (prev != &ht_[bucket_nr]) {
                    _unlockNode((Node*)prev, "ht.remove.prev");
                }
                node->htSetNext(nullptr);
                node->htSetFlag(false);

                break;
            }
            prev    = current;
            current = current->htNext();
        }

        unlockBucket(bucket_nr);
        assert(!node->htFlag());
        return true;
    }

    std::vector<Node>         data_;
    std::vector<NodeBase>     ht_;
    std::unique_ptr<lock_t[]> ht_locks_;
    Node                      lru_head_;
    Node                      lru_tail_;
    Node                      pool_head_;
    Node                      pool_tail_;

    typename config::hasher_t        hasher_;
    typename config::deletion_policy deleter_;
    typename config::profile_stats_t profile_stats_;
};

template <typename Config, unsigned int IgnoreBitsInHash>
void ConcurrentLRU<Config, IgnoreBitsInHash>::assertIsCoherent() {
    // all nodes in pool have unset flags not locked and value is deinitialized
    // all nodes in lru have unset flags not locked and value is initialized
    // all nodes in lru are in ht
    std::cerr << "pool: ";
    dumpList(&pool_head_);
    std::cerr << "\n";
    std::cerr << "lru:  ";
    dumpList(&lru_head_);
    std::cerr << "\n";
    profileStats().print(std::cerr);
    // dumpHt();
    std::cerr << "\n";

    bool raise = false;

    std::set<Node*> unseen_nodes;
    for (auto& n : data_) {
        unseen_nodes.insert(&n);
    }

    Node* node = &pool_head_;
    Node* prev = node;
    while (true) {
        if (node != &pool_head_ && node != &pool_tail_ &&
            !(node >= data_.data() && node < data_.data() + data_.size())) {
            std::cerr << "Unknown pool node: " << dumpNode(node) << ", parent: " << dumpNode(prev)
                      << std::endl;
            raise = true;
            break;
        }
        if (!(!node->lruFlag() && !node->isLocked() && !node->htFlag() &&
              node->htNext() == nullptr)) {
            std::cerr << "A node in the pool is not coherent: " << dumpNode(node) << std::endl;
            raise = true;
        }
        if (!node->lru_next) {
            if (node != &pool_tail_) {
                std::cerr << "Last pool node is not the tail!\n";
                raise = true;
            }
            break;
        }
        unseen_nodes.erase(node);
        prev = node;
        node = node->lru_next;
    }

    std::set<Node*> lru_nodes;
    node = &lru_head_;
    prev = node;
    while (true) {
        if (node != &lru_head_ && node != &lru_tail_ &&
            !(node >= data_.data() && node < data_.data() + data_.size())) {
            std::cerr << "Unknown lru node:  " << dumpNode(node) << ", parent: " << dumpNode(prev)
                      << std::endl;
            raise = true;
            break;
        }

        if (!(node->lruFlag() && !node->isLocked() && node->htFlag())) {
            std::cerr << "A node in the LRU is not coherent: " << dumpNode(node) << std::endl;
            raise = true;
        }
        lru_nodes.insert(node);
        unseen_nodes.erase(node);
        if (!node->lru_next) {
            if (node != &lru_tail_) {
                std::cerr << "Last LRU node is not the tail!\n";
                raise = true;
            }
            break;
        }

        node = node->lru_next;
    }

    if (!unseen_nodes.empty()) {
        std::cerr << "Unsynchronized nodes:\n";
        for (auto n : unseen_nodes) {
            std::cerr << "  ";
            dumpList(n);
            std::cerr << '\n';
        }

        raise = true;
    }
    std::set<Node*> ht_seen_all;
    for (auto& bucket : ht_) {
        node = reinterpret_cast<Node*>(bucket.htNext());
        std::set<Node*> ht_seen;
        while (node) {
            if (lru_nodes.count(node) == 0) {
                std::cerr << "Node not in the LRU: " << dumpNode(node) << std::endl;
                raise = true;
                break;
            }
            if (ht_seen.count(node)) {
                std::cerr << "Loop in the bucket: " << dumpNode(node) << std::endl;
                raise = true;
                break;
            }
            if (ht_seen_all.count(node)) {
                std::cerr << "Node in multiple buckets: " << dumpNode(node) << std::endl;
                raise = true;
            }
            ht_seen.insert(node);
            ht_seen_all.insert(node);
            node = reinterpret_cast<Node*>(node->htNext());
        }
    }

    if (raise) {
        std::raise(SIGINT);
    }
}

template <typename Config, unsigned int IgnoreBitsInHash>
void ConcurrentLRU<Config, IgnoreBitsInHash>::dump() {
    std::cout << "LRU:\n  ";
    Node* node = &pool_head_;
    while (node) {
        std::cout << " -> " << dumpNode(node);
        node = node->lru_next;
    }
    std::cout << "\n  ";

    node = &lru_head_;
    while (node) {
        std::cout << " -> " << dumpNode(node);
        node = node->lru_next;
    }
    std::cout << "\n\n";
}

template <typename Config, unsigned int IgnoreBitsInHash>
void ConcurrentLRU<Config, IgnoreBitsInHash>::dumpList(ConcurrentLRU::Node* node) {
    bool            first = true;
    std::set<Node*> seen;
    char            name[100];
    while (node) {
        if (first) {
            first = false;
        } else {
            std::cerr << "::";
        }
        dumpNodeExt(name, node, false);
        std::cerr << name;
        if (seen.count(node)) {
            std::cerr << "...";
            break;
        }
        seen.insert(node);
        node = node->lru_next;
    }
}

template <typename Config, unsigned int IgnoreBitsInHash>
std::string ConcurrentLRU<Config, IgnoreBitsInHash>::dumpNode(ConcurrentLRU::Node* node) {
    if (node == nullptr) {
        return "NULL";
    }
    std::string name;
    if (node == &lru_head_) {
        name = "lru_head";
    } else if (node == &lru_tail_) {
        name = "lru_tail";
    } else if (node == &pool_head_) {
        name = "pool_head";
    } else if (node == &pool_tail_) {
        name = "pool_tail";
    } else {
        auto idx = node - data_.data();
        if (idx < data_.size()) {
            name = std::to_string(idx);
        } else {
            char buf[80];
            sprintf(buf, "%p", node);
            name = buf;
            return name + "[k=?|m=?|l=?|ht=?]";
        }
    }

    name = name + "[m=" + std::to_string(int(node->isLocked())) +
           "|l=" + std::to_string(int(node->lruFlag())) +
           "|ht=" + std::to_string(int(node->htFlag())) + "]";
    return name;
}

template <typename Config, unsigned int IgnoreBitsInHash>
void ConcurrentLRU<Config, IgnoreBitsInHash>::dumpHt() {
    std::cerr << "HT:\n";
    for (size_t i = 0; i < ht_.size(); i++) {
        std::cerr << "  [" << i << "]:";

        Node*           node = reinterpret_cast<Node*>(ht_[i].htNext());
        std::set<Node*> seen;
        char            buf[100];
        while (node) {
            dumpNodeExt(buf, node, false);
            std::cerr << ' ' << buf;
            if (seen.count(node)) {
                std::cerr << " ...";
                break;
            }
            seen.insert(node);
            node = reinterpret_cast<Node*>(node->htNext());
        }
        std::cerr << "\n";
    }
}

template <typename Config, unsigned int IgnoreBitsInHash>
void ConcurrentLRU<Config, IgnoreBitsInHash>::dumpNodeExt(char*                      buf,
                                                          const ConcurrentLRU::Node* node,
                                                          bool print_addr) const {
    const char* name = "";
    char        idx_buf[100];
    if (node == nullptr) {
        name = "NULL";
    } else if (node == &lru_head_) {
        name = "lru_head";
    } else if (node == &lru_tail_) {
        name = "lru_tail";
    } else if (node == &pool_head_) {
        name = "pool_head";
    } else if (node == &pool_tail_) {
        name = "pool_tail";
    } else {
        auto idx = node - data_.data();
        if (idx < data_.size()) {
            sprintf(idx_buf, "%zd", idx);
            name = idx_buf;
        } else {
            name = "?";
        }
    }
    if (print_addr) {
        sprintf(buf, "%s<%p>", name, node);
    } else {
        sprintf(buf, "%s", name);
    }
}
