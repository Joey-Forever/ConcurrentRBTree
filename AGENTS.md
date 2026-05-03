# ConcurrentRBTree Agent Guide

This file gives coding agents the project-specific context needed to make safe changes in this repository.

## Project Snapshot

`gipsy_danger::ConcurrentRBTree` is a header-only C++ concurrent red-black tree implementation for read-heavy workloads. The main library lives in [src/include/ConcurrentRBTree.h](src/include/ConcurrentRBTree.h). It exposes a `std::set`-like API through `ConcurrentRBTree<VALUE>::Accessor` and is intended to be a practical alternative to `folly::ConcurrentSkipList` for workloads dominated by reads.

The implementation is performance-sensitive and concurrency-sensitive. Treat algorithmic and memory-ordering changes as high risk.

## Repository Layout

- [src/include/ConcurrentRBTree.h](src/include/ConcurrentRBTree.h): the full header-only implementation.
- [src/test/accuracy_test](src/test/accuracy_test): correctness, edge-case, type, leak, and concurrency tests.
- [src/test/comparision_test](src/test/comparision_test): benchmark harnesses and scripts comparing against `folly::ConcurrentSkipList`.
- [src/test/comparision_test/x86_result](src/test/comparision_test/x86_result): checked-in benchmark result images.
- [docs/architecture](docs/architecture): architecture diagrams.
- [CLAUDE.md](CLAUDE.md): existing Claude-oriented project notes; keep it consistent with this file when changing project conventions.

## Build And Test

Accuracy tests:

```bash
g++ -std=c++17 -Wall -Wextra -O2 -g -Isrc/include \
  src/test/accuracy_test/rbtree_edge_case_test.cpp \
  -o /tmp/rbtree_edge_case_test -pthread
/tmp/rbtree_edge_case_test
```

RBTree performance benchmark:

```bash
g++ -std=c++17 -pthread -O2 -DNDEBUG -Isrc/include \
  src/test/comparision_test/concurrent_rbtree_perf_test.cpp \
  -o /tmp/concurrent_rbtree_perf_test
/tmp/concurrent_rbtree_perf_test
```

The skiplist comparison benchmark requires Folly and environment-specific include/link paths.

For direct downstream usage, the library itself is header-only and requires only the include path:

```bash
g++ -std=c++17 -I/path/to/ConcurrentRBTree/src/include your_code.cpp -o your_program
```

Use `-DNDEBUG` for performance-oriented builds because `RB_ASSERT` is disabled only when `NDEBUG` is defined.

## Core Architecture

The implementation maintains two indexes over the same nodes:

- Red-black tree: approximate `O(log n)` search index. Tree child pointers are atomic and commonly accessed with relaxed/no-barrier ordering.
- Sorted linked list: authoritative ordered view used by iterators and final lookup validation.

Important sentinel nodes:

- `root_`: virtual tree root; the real tree root is `root_->leftSonNoBarrier()`.
- `list_header_`: beginning sentinel for the sorted list.
- `list_tailer_`: ending sentinel for the sorted list.

The sorted linked list is the source of truth for visible order. Do not make tree-only changes that can leave list order, accessibility, or iterator behavior inconsistent.

## Concurrency Model

Reads are intended to be lock-free:

- Readers use the tree to find an approximate position.
- They then walk the sorted list to validate the exact result.
- If a concurrent rotation or update invalidates the approximation, lookup retries from the tree root.

Writes use a two-phase design:

- Phase 1: find an estimated position without taking the writer lock.
- Phase 2: acquire `write_leader_flag_`, refine through the sorted list, mutate the list/tree, then release the flag.

Node visibility is controlled with `accessible_`:

- Inserted nodes must be fully wired before becoming accessible.
- Erased nodes must become inaccessible before detachment/recycling.
- Iterators use accessible-next behavior to skip nodes being erased.

Memory reclamation is handled by `NodeRecycler` and `Accessor` reference tracking. `Accessor` instances act as epoch guards via `addRef()`/`releaseRef()`. Do not delete erased nodes directly from normal erase paths.

## Invariants To Preserve

- Values are unique; duplicate `insert` returns the existing node and `false`.
- `VALUE` currently needs `operator<`, `operator==`, default construction, and move construction.
- Custom comparators and const iterators are not currently supported.
- `size_` must match the number of accessible data nodes after completed writes.
- The sorted linked list must remain strictly ordered and must include every accessible data node.
- Red-black tree color/rotation invariants must hold after completed structural writes.
- `list_header_`, `list_tailer_`, and `root_` are internal sentinels and must never be returned as user-visible values.
- Atomic ordering is intentional. Do not strengthen or weaken memory orders without explaining the correctness argument and measuring likely performance impact.

## Editing Guidelines

- Keep the library header-only unless the user explicitly asks for a larger restructuring.
- Prefer small, targeted patches. Avoid unrelated formatting churn in [src/include/ConcurrentRBTree.h](src/include/ConcurrentRBTree.h).
- Preserve public API compatibility with the current `Accessor` pattern unless the task is specifically an API redesign.
- Add or update focused tests in [src/test/accuracy_test](src/test/accuracy_test) for correctness-sensitive behavior.
- For concurrency changes, include tests that exercise mixed reads/writes when practical.
- Benchmark-only edits should avoid changing correctness tests or public API.
- The directory name is currently spelled `comparision_test`; do not rename it as part of unrelated work.

## Verification Expectations

For implementation changes, run at least:

```bash
g++ -std=c++17 -Wall -Wextra -O2 -g -Isrc/include \
  src/test/accuracy_test/rbtree_edge_case_test.cpp \
  -o /tmp/rbtree_edge_case_test -pthread
/tmp/rbtree_edge_case_test
```

For performance-sensitive changes, also run or provide clear direct compiler commands for:

```bash
g++ -std=c++17 -pthread -O2 -DNDEBUG -Isrc/include \
  src/test/comparision_test/concurrent_rbtree_perf_test.cpp \
  -o /tmp/concurrent_rbtree_perf_test
```

If dependencies are unavailable, report exactly which command failed and why.

## Known Caveats

- The performance tests depend on local Folly/Benchmark installation details and may need environment-specific include/link paths.
- Some tests use large data volumes and may take a while or require substantial memory.
- The benchmark result images are documentation artifacts. Do not regenerate or replace them unless benchmarking output is part of the requested change.
