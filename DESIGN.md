# Level-2 Order Book Design

## Build and run

```bash
g++ -O3 -march=native -pthread -std=c++20 main.cpp -o orderbook_bench
./orderbook_bench              # defaults: 10,000,000 events, 8 producers, 4 readers
./orderbook_bench 1000000 8 4  # smaller run
```

## Architecture

- Producers create `OrderEvent` values and push them into `MPSCQueue<OrderEvent>`.
- The matching thread is the only thread that mutates `OrderBook`.
- Reader threads never touch the mutable book. They repeatedly load immutable `BookSnapshot` instances from `SnapshotPublisher`.
- Shutdown is RAII-driven: `MatchingEngine::~MatchingEngine()` calls `stop()`, enqueues a shutdown event, wakes the matcher, drains remaining events, publishes a final snapshot, and joins the thread.

This design keeps the hot matching path single-writer and deterministic. Producers race only to enqueue work; the matching thread assigns a monotonically increasing sequence number in consumption order.

## Data structures

- `std::map<Price, PriceLevel, std::greater<Price>> bids_` keeps the highest bid at `begin()`.
- `std::map<Price, PriceLevel, std::less<Price>> asks_` keeps the lowest ask at `begin()`.
- `PriceLevel` stores aggregate quantity and a `std::list<Order*>` for FIFO time priority.
- `std::unordered_map<OrderID, Order*> orderIndex_` is the required fast ID lookup.
- `std::unordered_map<OrderID, OrderHandle> handles_` stores side, price, and list iterator for O(1) cancellation once the order is found.
- `std::unordered_map<OrderID, std::unique_ptr<Order>> orderStorage_` owns order lifetime while preserving stable pointers.

`std::list` is used instead of a custom intrusive list to keep the implementation clear and exception-safe. A production implementation would usually make `Order` intrusive and allocate from a pool.

## Matching behavior

- Limit buy orders match while `bestAsk <= limitPrice`.
- Limit sell orders match while `bestBid >= limitPrice`.
- Market buys match until quantity is exhausted or asks are empty.
- Market sells match until quantity is exhausted or bids are empty.
- Partial fills reduce the resting front order and price-level aggregate.
- Full fills remove the resting order from the FIFO, ID index, handle map, and owning storage.
- Empty price levels are erased immediately.
- Modifying to a lower/equal quantity at the same price preserves priority.
- Modifying price or increasing quantity cancels and re-adds, which loses priority like many exchange rulebooks.

## API complexity

- `addOrder`: O(M + log P) where `M` is the number of resting orders consumed and `P` is price-level count. If it only rests at an existing level, insertion is O(1) after map lookup; a new level is O(log P).
- `cancelOrder`: average O(1) ID lookup plus O(1) list erase, with O(log P) only if the level becomes empty and must be erased from `std::map`.
- `modifyOrder`: average O(1) for same-price quantity reduction; otherwise cancel plus add complexity.
- `executeMarketOrder`: O(M + E log P), where `M` is consumed orders and `E` is erased empty levels.
- `bestBid` / `bestAsk`: O(1), because best levels are map `begin()`.
- `spread` / `midPrice`: O(1).
- `getDepth(levels)`: O(levels).
- `printBook(levels)`: O(levels).
- `makeSnapshot(levels)`: O(levels).

## Synchronization choices

- `MPSCQueue` uses an atomic linked-list head. Producers publish nodes with `exchange(acq_rel)` and link via `next.store(release)`. The single consumer reads `next.load(acquire)`, which observes fully initialized events.
- A `condition_variable` is used only to park the matching thread when the queue is empty. It is not used to protect book state.
- `running_` uses acquire/release so shutdown intent is observed consistently across threads.
- Statistics use relaxed atomics because they are telemetry; exact inter-thread ordering is not required.
- `SnapshotPublisher` uses `std::atomic<std::shared_ptr<const BookSnapshot>>`. The store is release and load is acquire, so readers see a fully initialized immutable snapshot.
- `alignas(64)` is used on frequently touched atomics/stat blocks to reduce false sharing.

## Bottlenecks

- `new` per queue node and `unique_ptr<Order>` per resting order create allocator pressure.
- `std::map` has pointer-heavy red-black tree nodes and weaker cache locality than a flat ladder or radix/tree hybrid.
- `std::list` gives stable iterators but has poor locality.
- `atomic<shared_ptr>` incurs reference-count traffic for readers.
- Snapshot publication copies top depth levels periodically.
- A single matching thread preserves determinism but caps throughput per instrument.
- Console timing includes producer submission and final drain, not just core matching cycles.

## Real HFT optimizations

- Replace the linked MPSC queue with a bounded lock-free ring buffer and batch dequeue.
- Use memory pools/slab allocators for events, orders, and price levels.
- Use intrusive FIFO links embedded directly in `Order`.
- Use integer ticks and compact arrays for dense price ladders.
- Partition by symbol so each instrument or shard has its own matching core.
- Pin producer, matcher, and feed threads to specific CPUs.
- Use NUMA-local allocation and avoid cross-socket memory traffic.
- Enable huge pages for large arenas.
- Use hardware timestamping and cycle counters for latency measurement.
- Use RCU/epoch snapshots with raw pointer publication to avoid shared-pointer reference-count traffic.
- Batch snapshot generation or publish deltas instead of full depth copies.
- Tune cache-line layout and prefetch likely next orders during matching.
- Avoid exceptions and RTTI in the hot path in production builds.

## Verified run

On this machine:

```text
Processed events: 10000000
Submitted events: 10000000
Throughput: 1283114.19 orders/sec
Elapsed: 7.79 sec
Average submit latency: 2401.39 ns
Max submit latency: 10923700 ns
Filled quantity: 1621459221
Live orders: 961332
Snapshots published: 154
Snapshot reads: 20966480
Book invariant check: PASS
Best bid: 9995
Best ask: 10036
Spread: 41
Mid: 10015.50
```

