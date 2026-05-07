# ConcurrentRBTree Project TODO

Source session: `/Users/caijiajian/.codex/sessions/2026/05/01/rollout-2026-05-01T15-55-08-019de288-c948-7381-bea3-6ee51d77ad89.jsonl`

This document records the project direction, benchmark findings, and follow-up tasks discussed in the current Codex session. It is intended as a project roadmap, not a verbatim transcript.

## Product Direction

The strongest positioning for this project is not a general-purpose point-key cache. The stronger positioning is:

> A Raft-backed, strongly consistent, in-memory ordered index service optimized for real-time range scan and stable ordered cursors.

The core differentiator is:

- `ConcurrentRBTree` provides stable ordered iterators under concurrent insert/erase.
- Raft can provide fault tolerance, committed write ordering, and linearizable read admission.
- The service should optimize live ordered range views, not just `Get(key)`.

Recommended wording:

- Strongly consistent in-memory ordered index service.
- Real-time range scan database.
- Stable ordered successor cursor service.
- Read-mostly live ordered view over committed data.

Avoid positioning it as:

- A plain Redis-compatible cache.
- A general KV store optimized only for point lookup.
- A replacement for MySQL/PostgreSQL as a system of record.

## Core Semantic Decision

The desired range scan behavior is live, not snapshot-based.

Required semantics:

- Point `Get`, `Put`, and `Delete` should be linearizable within a Raft shard.
- A range request should first pass a Raft ReadIndex or leader lease check so the serving replica is not stale.
- During a range scan, the iterator may reflect newly applied committed inserts and erases.
- Range scan does not need to represent a single fixed snapshot revision.
- Stable cursor must remain valid while concurrent writes apply.
- Returned values must come from committed/applied state.

Document this explicitly as:

```text
Point operations are linearizable.
Range scan provides a stable live ordered cursor over committed data.
Range scan may reflect writes committed during scanning and does not provide snapshot isolation.
```

## Key Product Feature: Track Cursor

The most distinctive feature should be a server-side ordered cursor anchored at one object.

Concept:

```text
OpenCursor(anchor_value, direction, limit)
PollCursor(cursor_id, limit)
SubscribeCursor(cursor_id, limit)
AdvanceCursor(cursor_id, steps)
CloseCursor(cursor_id)
```

Difference from etcd:

```text
etcd Range + Watch anchors a key range and outputs object change events.
This project anchors one object's ordered position and outputs the real-time ordered successor view.
```

Example:

```text
Current ordered set: 100, 110, 120, 130
anchor = 100
limit = 3
result = 110, 120, 130

After DELETE 120, PUT 115, PUT 140:
result = 110, 115, 130
```

etcd would return events. This service should return the updated successor window directly.

## Cursor Requirements

Implement cursor lifecycle controls before exposing long-lived server-side iterators:

- Cursor TTL.
- Client heartbeat or lease.
- Max cursors per client.
- Max cursor lifetime.
- Memory quota for pinned iterators.
- Explicit `CloseCursor`.
- Cursor invalidation on shard epoch or leader term changes.
- Simple first version: return `CURSOR_EXPIRED` after leader change and require reopening from `anchor_value`.

Cursor metadata should include at least:

```text
cursor_id
shard_id
anchor_value
direction
leader_term
shard_epoch
created_at
last_access_at
```

## Raft Integration TODO

Design the replicated service around range-sharded Raft groups.

Tasks:

- Define shard key ranges.
- Map each shard to one Raft group.
- Route writes to the Raft leader.
- Apply committed log entries to `ConcurrentRBTree`.
- Keep `applied_index` per shard.
- Implement ReadIndex-based leader reads first.
- Add lease reads only after clock and lease safety rules are clearly documented.
- Define follower read behavior:
  - either disallow initially,
  - or follower asks leader for ReadIndex and waits for `applied_index >= read_index`.
- Define failure behavior for open cursors during leader change.
- Define snapshot/install-snapshot recovery for in-memory tree state.

For the current data structure, the existing single-writer design is acceptable because Raft apply for one shard is already naturally ordered.

## Data Model TODO

Initial target should be ordered set/index, not full SQL.

Recommended first model:

```text
Put(key, value)
Delete(key)
Get(key)
Range(start, limit)
OpenCursor(anchor, limit)
PollCursor(cursor_id, limit)
CloseCursor(cursor_id)
```

Potential indexed use cases:

- Real-time leaderboard around one user.
- Time-ordered feed/timeline cursor.
- TTL/expiration queue.
- Delay task queue.
- Order book price-level tracking.
- Strongly consistent in-memory secondary index for a database.
- Permission/risk/config ordered effective-time views.

## Consistency Documentation TODO

Write a formal consistency document covering:

- Linearization point for Raft writes.
- Linearizable point reads.
- Live range scan semantics.
- Stable cursor semantics.
- What happens to elements inserted before the anchor during cursor polling.
- What happens to elements inserted after the anchor but before the current successor window.
- What happens when the anchor value itself is deleted.
- Difference between live cursor and snapshot cursor.

Recommended first rule for anchor deletion:

```text
If the anchor is deleted after cursor creation, the cursor remains at the anchor's former logical position until closed or expired.
```

## Comparison And Benchmark TODO

Current benchmark work added Masstree comparison support:

- `src/test/comparision_test/concurrent_masstree_perf_test.cpp`
- `src/test/comparision_test/masstree_config_compat.h`
- `src/test/comparision_test/x86_result/throughput_16threads_4000000init_masstree.jpg`
- `src/test/comparision_test/x86_result/throughput_10threads_4000000init_masstree.jpg`

Benchmark findings:

- Masstree is significantly faster for mixed point `find/insert/erase` workload.
- This is expected because Masstree is cache-friendly and supports fine-grained concurrent writes.
- `ConcurrentRBTree` should not compete primarily on point-key throughput.
- Future benchmarks should test the project's intended strength:
  - long range scan under concurrent writes,
  - stable cursor poll throughput,
  - repeated successor-window queries from an anchored object,
  - server-side cursor vs repeated `lower_bound`,
  - Redis ZSet repeated range/rank query comparison,
  - etcd Range+Watch plus client-maintained window comparison.

Benchmark TODO:

- Add a range scan benchmark to `src/test/comparision_test`.
- Add a cursor tracking benchmark:
  - initialize millions of ordered values,
  - create many cursors anchored at random values,
  - repeatedly poll next `N` successors,
  - run concurrent insert/erase workload,
  - measure poll latency and throughput.
- Add Redis ZSet comparison if a local Redis is available.
- Add etcd Range+Watch conceptual or real comparison only if dependency setup is practical.
- Fix latency columns in `test_perf.h`; they currently print `0.00` because latency accounting is commented out.

## Masstree Comparison Notes

Masstree is a map/KV structure, not a set.

Current comparison alignment:

- Masstree uses `4B key + 4B value`.
- ConcurrentRBTree benchmark uses an 8-byte wrapper value:

```cpp
struct Int64SetValue {
  int32_t key;
  uint32_t payload;
};
```

Important benchmark interpretation:

- The Masstree benchmark tests point operations, not stable iterators.
- It does not measure the primary intended advantage of `ConcurrentRBTree`.
- Keep Masstree comparison as a baseline, but do not use it as the main product proof.

## Documentation TODO

Add or update docs explaining:

- Why this project is not just Redis ZSet.
- Why this project is not etcd Range+Watch.
- Why this project is not Elasticsearch.
- Why MySQL/PostgreSQL remain the system of record in many deployments.
- Where this service sits:

```text
MySQL/PostgreSQL: durable system of record.
Elasticsearch: full-text search and analytics, near real-time.
Redis ZSet: high-performance ordered set, usually not Raft-linearizable.
etcd: strongly consistent coordination KV with Range+Watch event stream.
ConcurrentRBTree service: strongly consistent live ordered successor view.
```

## API Design TODO

Draft API definitions:

```text
Put(key, value)
Delete(key)
Get(key)
Range(start_key, limit, consistency=live)
OpenCursor(anchor_key, direction, limit)
PollCursor(cursor_id, limit)
SubscribeCursor(cursor_id, limit)
AdvanceCursor(cursor_id, steps)
CloseCursor(cursor_id)
```

Define response payloads:

```text
values
cursor_id
leader_term
applied_index
has_more
cursor_expired
```

## Implementation Caution

Do not force fine-grained multi-writer red-black-tree writes unless there is a strong reason. The session analysis concluded:

- Red-black-tree rotations and rebalancing are not a good fit for strict fine-grained concurrent writes.
- The current single-writer design is reasonable for read-mostly workloads.
- Raft apply naturally serializes writes per shard, which matches the current implementation.

Prioritize:

- Stable iterator correctness.
- Memory reclamation under long-lived cursors.
- Clear live range scan semantics.
- Cursor resource control.
- Range/cursor benchmarks.

## Immediate Next Steps

- [ ] Decide whether to keep the Masstree benchmark files in the repository.
- [ ] Add range scan benchmark focused on stable iterators.
- [ ] Add cursor tracking benchmark.
- [ ] Fix latency measurement in `test_perf.h`.
- [ ] Write `docs/positioning.md` for product positioning.
- [ ] Write `docs/consistency.md` for point/read/range/cursor semantics.
- [ ] Draft service API in `docs/api.md`.
- [ ] Decide Raft library or implementation plan.
- [ ] Define cursor lifetime and memory reclamation policy.
- [ ] Add tests for iterator/cursor behavior when the anchor is erased.
- [ ] Add tests for insertions between anchor and current successor window.
- [ ] Add tests for concurrent erase while cursor is polling successors.
