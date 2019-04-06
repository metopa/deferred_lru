# DeferredLRU


DeferredLRU is a novel highly scalable concurrent LRU cache.
It uses a concurrent hash table for fast item lookup and
a doubly linked list for tracking access order.
It uses a different approach on tracking the item access order
compared to a regular LRU cache.
Instead of being moved immediately they are added
to another linked list, called Recent.
When the number of nodes in the \Recent list hits some threshold,
a single thread performs the PullRecent operation.
It evicts all the nodes in the Recent list from the LRU list and
reinserts them into the head in a single step.

This trick vastly decreases the total number of head insertions
and improves cache scalability.
DeferredLRU scales well with a number of threads. The achieved speedup
on 32 threads is about 29 for the lookup test
and 24 for the lookup-evict-insert test.

## Structure


### Node pool
The total cache capacity is known beforehand and fixed, so that all nodes
are preallocated during the initialization stage.
DeferredLRU does not perform any dynamic allocation after this.
All empty nodes are stored in a node pool --
a singly linked list. When a new node
is requested, the head node in the pool is removed.

### Item lookup
DeferredLRU relies on a hash table with open hashing for item lookup.
It is represented as an array of buckets, where each bucket is
a head of a linked list. Each hash table bucket has its own mutex.

### Access order
DeferredLRU tracks the access order quite similarly to a typical LRU cache.
All items form a doubly linked list, new nodes are added
to the head, and the tail nodes are considered least recently used.
The difference is in the way the accessed nodes are moved to the head.
When a node is visited, the caller appends it to the Recent list
(if the node has not been added already) instead
of immediately pulling the node to the head.

### Recent list
Recently accessed nodes are added to the Recent list.
It is a singly linked list. DeferredLRU container keeps a link to
the Recent list head and the current number of nodes in it.
Initially, the Recent list head points to some dummy terminal.
When the list is processed, reaching this node terminates the processing loop.

When a node is not in the Recent list,
its `recent_next` link is set to NULL.
Later, when it is accessed, if the node is not yet
in the Recent list, it is appended to it. With this approach,
it is always possible to tell if a node is in the Recent list
by checking if the link is NULL or not.

When the number of nodes in the Recent list hits a certain threshold
(for example, 10% of the cache size), PullRecent operation is invoked.
It extracts all nodes in the Recent list from the LRU list and reinserts them in the head.

### Cache consolidation
Cache consolidation includes two optional stages:
- PullRecent, that resets and processes the Recent list.
    It is triggered when the Recent list grows past the threshold.
- PurgeOld, that evicts a portion of least recently used nodes from
    the LRU list and puts them in the empty node pool.
    It is triggered when a new item is attempted
    to be inserted, but the cache is full.

A consolidation is triggered when any of these stages is triggered.
The triggering is similar for both stages. It is performed in two steps.
At first, an atomic binary flag is raised, signaling to whichever thread
would perform consolidation, that the corresponding operation was requested.
Then the triggering thread attempts to perform the consolidation by itself.
It tries to lock a mutex (using TryLock), that guards the consolidation.
If succeeded, the consolidation is performed.

The opposite case means that another thread is already performing
consolidation at the moment. In the case of triggering
PullRecent, no additional actions are required.
However, insertion can not proceed without an empty node.
Therefore, the thread would spin in a loop waiting for
either the consolidating thread to put a new node
to the empty node pool or the current consolidation to finish,
so that it can perform a new one by itself.

PullRecent atomically takes a slice
of the Recent list. All nodes in the slice are removed from the
LRU list and joined in a temporal list.
Finally, this temporal list is inserted into
the LRU list head in a single insertion.

PurgeOld is responsible for
evicting the least recently used nodes
and putting them into the empty node pool.
This is done by traversing the LRU list backward.
Every node (that is not marked as recent) is removed from its
bucket and then from the LRU list.

- Figure
  - Structure
  - After find
  - Consolidation
    - Purge
    - Pull 1
    - Pull 2

## Reference implementation

TBA

## Benchmark

TBA

## Thanks
