# L2 Order Book in C++20

A high-performance Level-2 limit order book written in modern C++20. It supports price-time priority matching, market and limit orders, order modification/cancellation, immutable market-depth snapshots, an interactive terminal mode, and stress benchmarking.

## Features

- Limit orders and market orders
- Buy and sell side books
- FIFO priority inside each price level
- Partial and full fills
- Automatic removal of empty price levels
- O(1) order cancellation using order ID lookups and list iterators
- Real-time tracking of live buy/sell order counts and quantities
- Immutable snapshots for lock-free reader access
- Multithreaded benchmark with producer, matcher, and reader threads
- Interactive terminal mode for manually placing and matching orders
- Per-order matching latency measurement
- Random workload simulation from the terminal

## Build

```bash
g++ -O3 -march=native -pthread -std=c++20 main.cpp -o orderbook_bench
```

On Windows PowerShell with MinGW:

```powershell
g++ -O3 -march=native -pthread -std=c++20 main.cpp -o orderbook_bench
```

## Run the Benchmark

Default benchmark:

```bash
./orderbook_bench
```

Defaults:

- 10,000,000 generated events
- 8 producer threads
- 4 reader threads

Custom benchmark:

```bash
./orderbook_bench <events> <producer_threads> <reader_threads>
```

Example:

```bash
./orderbook_bench 1000000 8 4
```

The benchmark prints processed event count, throughput, submit latency, filled quantity, snapshot reads, live order counts, and an invariant check.

## Interactive Mode

Start the terminal order book:

```bash
./orderbook_bench interactive
```

PowerShell:

```powershell
.\orderbook_bench.exe interactive
```

Available commands:

```text
buy <id> <price> <qty>
sell <id> <price> <qty>
market <buy|sell> <qty> [id]
cancel <id>
modify <id> <price> <qty>
simulate <events> [seed]
book [levels]
summary
help
quit
```

Example session:

```text
orderbook> sell 1 101 50
No matches.
Filled: 0 | Resting: 50 | Unfilled: 0 | Processing time: 16800 ns

orderbook> buy 2 102 20
Matches:
  incoming 2 BUY matched resting 1 @ 101 qty 20
Filled: 20 | Resting: 0 | Unfilled: 0 | Processing time: 400 ns
```

## Simulate Orders Interactively

Inside interactive mode:

```text
simulate 100000 123
```

This generates random limit orders, market orders, modifies, and cancels, then prints:

- total simulated events
- number of trades generated
- throughput
- average processing time
- max processing time
- book invariant result
- current buy/sell totals
- best bid and ask

## Architecture

The benchmark architecture uses:

- multiple producer threads generating `OrderEvent` messages
- a lock-free MPSC queue for producer-to-matcher handoff
- one dedicated matching thread that owns all order book mutation
- multiple reader threads loading immutable snapshots without blocking the matcher

The mutable book is single-writer by design. This keeps matching deterministic and avoids coarse-grained locks in the hot path.

## Core Data Structures

- `std::map<Price, PriceLevel, std::greater<Price>>` for bids, highest price first
- `std::map<Price, PriceLevel, std::less<Price>>` for asks, lowest price first
- `std::list<Order*>` per price level for FIFO time priority
- `std::unordered_map<OrderID, Order*>` for fast order lookup
- `std::unordered_map<OrderID, OrderHandle>` for O(1) cancellation by iterator
- `std::atomic<std::shared_ptr<const BookSnapshot>>` for immutable snapshot publication

## Matching Rules

- Buy limit orders match against the lowest asks while `ask <= buy_limit`.
- Sell limit orders match against the highest bids while `bid >= sell_limit`.
- Market orders consume available liquidity until filled or the opposite side is empty.
- Resting orders are matched in FIFO order within each price level.
- Reducing quantity at the same price preserves priority.
- Increasing quantity or changing price cancels and re-adds the order, losing priority.

## File Layout

```text
ConcurrentQueue.hpp   lock-free MPSC queue
DESIGN.md             detailed design and complexity notes
MatchingEngine.hpp    multithreaded matching engine wrapper
Order.hpp             order/event types
OrderBook.hpp         core order book and matching logic
PriceLevel.hpp        aggregated price level storage
Snapshot.hpp          immutable book snapshot model
main.cpp              benchmark and interactive terminal mode
```

## Validation

The program verifies internal invariants after benchmark and interactive simulation runs:

- price-level aggregate quantity equals sum of resting orders
- order ID index and handle map match live orders
- buy/sell order counters match actual book contents
- buy/sell total quantities match actual book contents

Successful runs end with:

```text
Book invariant check: PASS
```

or, in interactive mode:

```text
Final invariant check: PASS
```

