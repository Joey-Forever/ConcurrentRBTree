# ConcurrentRBTree

## Project Overview

A header-only C++17 concurrent red-black tree library (`gipsy_danger::ConcurrentRBTree`) optimized for read-intensive workloads. Provides a thread-safe sorted associative container with unique keys (similar to `std::set`), achieving up to 1.7x throughput over `folly::ConcurrentSkipList` in read-heavy scenarios.

## Architecture

### Dual-Index Structure

The core design maintains two parallel data structures over the same set of nodes:

1. **Red-Black Tree** — Provides O(log n) lookup by key. Tree pointers (`left_son_`, `right_son_`) are `std::atomic` with relaxed ordering since the tree is an approximate index, not the source of truth.
2. **Sorted Linked List** — The authoritative ordered view of all data nodes. `next_` pointers use acquire/release semantics. Threaded through `list_header_` -> data nodes in sorted order -> `list_tailer_`.

A virtual `root_` sentinel node holds the actual tree root as its left child.

### Concurrency Model

- **Reads are lock-free**: Tree traversal finds an approximate position (`less_bound`), then walks the sorted linked list (up to `MAX_EXTRA_STEPS_FROM_NO_GREATER_BOUND_TO_LOWER_BOUND = 3` steps) to find the exact target. If the walk fails (due to concurrent rotation), it retries from the tree root.
- **Writes use a two-phase design**:
  - Phase 1 (lock-free): Traverse the tree to find an `estimated_less_bound`.
  - Phase 2 (serialized): Acquire `write_leader_flag_` (an `atomic_flag` spinlock with `yield()`), refine position via linked list, perform structural modification.
- **Node visibility**: Each node has an `atomic<bool> accessible_` flag. On insert, the node is fully wired before `accessible_` is set to `true`. On erase, `accessible_` is set to `false` before detachment.

### Memory Management

- **Epoch-based garbage collection** via `NodeRecycler` (inspired by `folly::ConcurrentSkipList::NodeRecycler`). Erased nodes are collected and batch-deleted only when all active `Accessor` instances (which act as epoch guards via `addRef`/`releaseRef`) are released.
- `Accessor` constructors call `addRef()`, destructors call `releaseRef()`. The last `releaseRef()` triggers batch deletion of recycled nodes.

### Key Implementation Details

- **False sharing prevention**: `place_holder_` arrays (20 pointers each) pad between `recycler_`, `write_leader_flag_`, and `size_` fields. `write_leader_flag_` and `size_` are `alignas(64)`.
- **Erase algorithm**: For non-leaf nodes with two children, the predecessor (right-most node of left subtree) is detached and used to replace the erased node in the tree, preserving sorted-list order. Single-child nodes are first rotated to become leaf nodes before deletion.
- **Rebalancing**: Standard red-black tree insert/erase rebalancing using rotations and color flips, but all tree pointer mutations use `NoBarrier` (relaxed) ordering since only the writer thread modifies the tree structure under the spinlock.

## Directory Structure

```
src/
  include/
    ConcurrentRBTree.h    # The entire library (header-only, ~1000 lines)
  test/
    accuracy_test/        # Correctness, edge-case, type, leak, and concurrency tests
      rbtree_single_thread_ability.cpp
      rbtree_concurrent_test.cpp
      rbtree_edge_case_test.cpp
      rbtree_leak_test.cpp
      rbtree_type_test.cpp
    comparision_test/     # Performance benchmarks vs folly::ConcurrentSkipList
      test_perf.h         # Benchmark harness
      concurrent_rbtree_perf_test.cpp
      concurrent_skiplist_perf_test.cpp
      plot_throughput.py   # Visualization script
      x86_result/          # Benchmark result images
```

## Build & Test

```bash
# Build and run an accuracy test directly
g++ -std=c++17 -Wall -Wextra -O2 -g -Isrc/include \
  src/test/accuracy_test/rbtree_edge_case_test.cpp \
  -o /tmp/rbtree_edge_case_test -pthread
/tmp/rbtree_edge_case_test

# Build the RBTree performance benchmark directly
g++ -std=c++17 -pthread -O2 -DNDEBUG -Isrc/include \
  src/test/comparision_test/concurrent_rbtree_perf_test.cpp \
  -o /tmp/concurrent_rbtree_perf_test
```

- Accuracy tests are standalone C++ programs under `src/test/accuracy_test`.
- The skiplist comparison benchmark requires Folly and environment-specific include/link paths.
- Use `-DNDEBUG` for production builds to disable `RB_ASSERT` assertions

## API

All access goes through `ConcurrentRBTree<VALUE>::Accessor`:

```cpp
auto tree = ConcurrentRBTree<int>::createInstance();  // returns shared_ptr
ConcurrentRBTree<int>::Accessor accessor(tree);
accessor.insert(42);
auto it = accessor.find(42);
accessor.erase(42);
auto lb = accessor.lower_bound(50);
for (auto it = accessor.begin(); it != accessor.end(); ++it) { ... }
```

- `VALUE` must support `operator<`, `operator==`, default construction, and move construction.
- Custom comparators are not yet supported.
- `const_iterator` is not yet supported.
