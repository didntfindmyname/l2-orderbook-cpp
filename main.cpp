#include "MatchingEngine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct alignas(64) ProducerStats {
    std::uint64_t sent{};
    std::uint64_t maxSubmitLatencyNs{};
    long double totalSubmitLatencyNs{};
};

std::uint64_t parseArg(int argc, char** argv, int index, std::uint64_t fallback) {
    if (argc <= index) {
        return fallback;
    }
    char* end = nullptr;
    auto value = std::strtoull(argv[index], &end, 10);
    return end && *end == '\0' ? value : fallback;
}

OrderEvent makeRandomEvent(
    std::mt19937_64& rng,
    std::uint64_t producer,
    std::uint64_t localIndex,
    std::uint64_t producerCount,
    std::uint64_t perProducer) {
    std::uniform_int_distribution<int> typeDist(0, 99);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_int_distribution<int> priceDist(-100, 100);
    std::uniform_int_distribution<int> qtyDist(1, 1'000);
    std::uniform_int_distribution<std::uint64_t> producerDist(0, producerCount - 1);
    std::uniform_int_distribution<std::uint64_t> localIdDist(0, std::max<std::uint64_t>(1, perProducer) - 1);

    int type = typeDist(rng);
    OrderEvent event;
    event.side = sideDist(rng) == 0 ? Side::Buy : Side::Sell;
    event.price = 10'000 + priceDist(rng);
    event.quantity = static_cast<Quantity>(qtyDist(rng));

    if (type < 68) {
        event.eventType = EventType::NewOrder;
        event.orderType = OrderType::Limit;
        event.id = (producer + 1) * 100'000'000'000ULL + localIndex + 1;
    } else if (type < 75) {
        event.eventType = EventType::NewOrder;
        event.orderType = OrderType::Market;
        event.id = 0;
    } else if (type < 88) {
        event.eventType = EventType::ModifyOrder;
        event.orderType = OrderType::Limit;
        const auto targetProducer = producerDist(rng);
        event.id = (targetProducer + 1) * 100'000'000'000ULL + localIdDist(rng) + 1;
    } else {
        event.eventType = EventType::CancelOrder;
        const auto targetProducer = producerDist(rng);
        event.id = (targetProducer + 1) * 100'000'000'000ULL + localIdDist(rng) + 1;
    }

    return event;
}

void readerLoop(const MatchingEngine& engine, std::atomic<bool>& readersRunning, std::atomic<std::uint64_t>& reads) {
    while (readersRunning.load(std::memory_order_acquire)) {
        auto snapshot = engine.snapshot();
        if (snapshot) {
            [[maybe_unused]] auto bid = snapshot->bestBid();
            [[maybe_unused]] auto ask = snapshot->bestAsk();
            [[maybe_unused]] auto mid = snapshot->midPrice();
            reads.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::yield();
    }
}

std::string lower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool parseSide(const std::string& token, Side& side) {
    const std::string value = lower(token);
    if (value == "buy" || value == "bid" || value == "b") {
        side = Side::Buy;
        return true;
    }
    if (value == "sell" || value == "ask" || value == "s") {
        side = Side::Sell;
        return true;
    }
    return false;
}

void printHelp() {
    std::cout
        << "Commands:\n"
        << "  buy <id> <price> <qty>       place a limit buy order\n"
        << "  sell <id> <price> <qty>      place a limit sell order\n"
        << "  market <buy|sell> <qty> [id] place a market order\n"
        << "  cancel <id>                  cancel a resting order\n"
        << "  modify <id> <price> <qty>    modify a resting order\n"
        << "  simulate <events> [seed]     generate and process random orders\n"
        << "  book [levels]                print the order book\n"
        << "  summary                      print current buy/sell totals\n"
        << "  help                         show this menu\n"
        << "  quit                         exit\n";
}

void printReport(const OrderBook::ExecutionReport& report, std::uint64_t elapsedNs) {
    if (report.trades.empty()) {
        std::cout << "No matches.\n";
    } else {
        std::cout << "Matches:\n";
        for (const auto& trade : report.trades) {
            std::cout << "  incoming " << trade.aggressorOrderId
                      << " " << toString(trade.aggressorSide)
                      << " matched resting " << trade.restingOrderId
                      << " @ " << trade.price
                      << " qty " << trade.quantity << "\n";
        }
    }

    std::cout << "Filled: " << report.filled
              << " | Resting: " << report.resting
              << " | Unfilled: " << (report.requested - report.filled - report.resting)
              << " | Processing time: " << elapsedNs << " ns"
              << "\n";
}

void printSummary(const OrderBook& book) {
    const auto buys = book.buySummary();
    const auto sells = book.sellSummary();
    std::cout << "Buy orders: " << buys.orderCount << " | Buy quantity: " << buys.totalQuantity << "\n";
    std::cout << "Sell orders: " << sells.orderCount << " | Sell quantity: " << sells.totalQuantity << "\n";
    auto bid = book.bestBid();
    auto ask = book.bestAsk();
    std::cout << "Best bid: " << (bid ? std::to_string(*bid) : "-") << "\n";
    std::cout << "Best ask: " << (ask ? std::to_string(*ask) : "-") << "\n";
}

OrderEvent makeInteractiveRandomEvent(
    std::mt19937_64& rng,
    std::uint64_t index,
    std::uint64_t maxKnownOrders,
    OrderID baseOrderId) {
    std::uniform_int_distribution<int> typeDist(0, 99);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_int_distribution<int> priceDist(-50, 50);
    std::uniform_int_distribution<int> qtyDist(1, 500);
    std::uniform_int_distribution<std::uint64_t> knownIdDist(0, std::max<std::uint64_t>(1, maxKnownOrders) - 1);

    OrderEvent event;
    const int type = typeDist(rng);
    event.side = sideDist(rng) == 0 ? Side::Buy : Side::Sell;
    event.price = 10'000 + priceDist(rng);
    event.quantity = static_cast<Quantity>(qtyDist(rng));

    if (type < 70) {
        event.eventType = EventType::NewOrder;
        event.orderType = OrderType::Limit;
        event.id = baseOrderId + index;
    } else if (type < 78) {
        event.eventType = EventType::NewOrder;
        event.orderType = OrderType::Market;
        event.id = baseOrderId + 1'000'000'000ULL + index;
    } else if (type < 90) {
        event.eventType = EventType::ModifyOrder;
        event.id = baseOrderId + knownIdDist(rng);
    } else {
        event.eventType = EventType::CancelOrder;
        event.id = baseOrderId + knownIdDist(rng);
    }

    return event;
}

void runInteractiveSimulation(OrderBook& book, std::uint64_t events, std::uint64_t seed, std::uint64_t& sequence) {
    std::mt19937_64 rng(seed);
    const OrderID baseOrderId = 1'000'000'000ULL + sequence;
    std::uint64_t processed = 0;
    std::uint64_t trades = 0;
    std::uint64_t maxLatencyNs = 0;
    long double totalLatencyNs = 0.0;

    const auto wallStart = std::chrono::steady_clock::now();
    for (std::uint64_t i = 1; i <= events; ++i) {
        OrderEvent event = makeInteractiveRandomEvent(rng, i, i, baseOrderId);

        const auto before = std::chrono::steady_clock::now();
        OrderBook::ExecutionReport report;
        switch (event.eventType) {
        case EventType::NewOrder:
            if (event.orderType == OrderType::Market) {
                report = book.executeMarketOrder(event.side, event.quantity, event.id);
            } else {
                report = book.addOrder(event.id, event.side, event.price, event.quantity, ++sequence);
            }
            trades += report.trades.size();
            break;
        case EventType::ModifyOrder:
            report = book.modifyOrder(event.id, event.price, event.quantity, ++sequence);
            trades += report.trades.size();
            break;
        case EventType::CancelOrder:
            book.cancelOrder(event.id);
            break;
        case EventType::Shutdown:
            break;
        }
        const auto after = std::chrono::steady_clock::now();
        const auto latency = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
        maxLatencyNs = std::max(maxLatencyNs, latency);
        totalLatencyNs += latency;
        ++processed;
    }
    const auto wallFinish = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(wallFinish - wallStart).count();

    std::cout << "Simulated events: " << processed << "\n";
    std::cout << "Trades generated: " << trades << "\n";
    std::cout << "Throughput: " << std::fixed << std::setprecision(2)
              << (seconds > 0.0 ? processed / seconds : 0.0) << " events/sec\n";
    std::cout << "Average processing time: "
              << (processed == 0 ? 0.0 : static_cast<double>(totalLatencyNs / processed)) << " ns\n";
    std::cout << "Max processing time: " << maxLatencyNs << " ns\n";
    std::cout << "Invariant check: " << (book.verifyInvariants() ? "PASS" : "FAIL") << "\n";
    printSummary(book);
}

int runInteractive() {
    OrderBook book(100'000);
    std::uint64_t sequence = 0;
    OrderID nextMarketId = 900'000'000'000ULL;

    std::cout << "Interactive L2 order book\n";
    printHelp();

    std::string line;
    while (true) {
        std::cout << "\norderbook> ";
        if (!std::getline(std::cin, line)) {
            break;
        }

        std::istringstream input(line);
        std::string command;
        input >> command;
        command = lower(command);
        if (command.empty()) {
            continue;
        }

        if (command == "quit" || command == "exit") {
            break;
        }

        if (command == "help") {
            printHelp();
            continue;
        }

        if (command == "book") {
            std::size_t levels = 10;
            input >> levels;
            book.printBook(levels);
            continue;
        }

        if (command == "summary") {
            printSummary(book);
            continue;
        }

        if (command == "buy" || command == "sell") {
            OrderID id{};
            Price price{};
            Quantity quantity{};
            if (!(input >> id >> price >> quantity)) {
                std::cout << "Usage: " << command << " <id> <price> <qty>\n";
                continue;
            }

            Side side = command == "buy" ? Side::Buy : Side::Sell;
            const auto before = std::chrono::steady_clock::now();
            auto report = book.addOrder(id, side, price, quantity, ++sequence);
            const auto after = std::chrono::steady_clock::now();
            const auto elapsedNs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
            printReport(report, elapsedNs);
            book.printBook(5);
            continue;
        }

        if (command == "market") {
            std::string sideToken;
            Quantity quantity{};
            OrderID id{};
            if (!(input >> sideToken >> quantity)) {
                std::cout << "Usage: market <buy|sell> <qty> [id]\n";
                continue;
            }
            if (!(input >> id)) {
                id = ++nextMarketId;
            }

            Side side{};
            if (!parseSide(sideToken, side)) {
                std::cout << "Side must be buy or sell.\n";
                continue;
            }

            const auto before = std::chrono::steady_clock::now();
            auto report = book.executeMarketOrder(side, quantity, id);
            const auto after = std::chrono::steady_clock::now();
            const auto elapsedNs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
            printReport(report, elapsedNs);
            book.printBook(5);
            continue;
        }

        if (command == "cancel") {
            OrderID id{};
            if (!(input >> id)) {
                std::cout << "Usage: cancel <id>\n";
                continue;
            }
            std::cout << (book.cancelOrder(id) ? "Canceled.\n" : "Order not found.\n");
            book.printBook(5);
            continue;
        }

        if (command == "modify") {
            OrderID id{};
            Price price{};
            Quantity quantity{};
            if (!(input >> id >> price >> quantity)) {
                std::cout << "Usage: modify <id> <price> <qty>\n";
                continue;
            }
            const auto before = std::chrono::steady_clock::now();
            auto report = book.modifyOrder(id, price, quantity, ++sequence);
            const auto after = std::chrono::steady_clock::now();
            const auto elapsedNs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
            printReport(report, elapsedNs);
            book.printBook(5);
            continue;
        }

        if (command == "simulate") {
            std::uint64_t events{};
            std::uint64_t seed = 0xC0FFEEULL;
            if (!(input >> events)) {
                std::cout << "Usage: simulate <events> [seed]\n";
                continue;
            }
            input >> seed;
            runInteractiveSimulation(book, events, seed, sequence);
            continue;
        }

        std::cout << "Unknown command. Type help.\n";
    }

    std::cout << "Final invariant check: " << (book.verifyInvariants() ? "PASS" : "FAIL") << "\n";
    return book.verifyInvariants() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && lower(argv[1]) == "interactive") {
        return runInteractive();
    }

    const std::uint64_t totalOrders = parseArg(argc, argv, 1, 10'000'000ULL);
    const std::uint64_t producerCount = parseArg(argc, argv, 2, 8);
    const std::uint64_t readerCount = parseArg(argc, argv, 3, 4);
    const std::uint64_t perProducer = (totalOrders + producerCount - 1) / producerCount;

    MatchingEngine engine(static_cast<std::size_t>(std::min<std::uint64_t>(totalOrders, 5'000'000ULL)), 10, 65'536);
    engine.start();

    std::atomic<bool> readersRunning{true};
    std::atomic<std::uint64_t> snapshotReads{0};
    std::vector<std::thread> readers;
    readers.reserve(readerCount);
    for (std::uint64_t i = 0; i < readerCount; ++i) {
        readers.emplace_back(readerLoop, std::cref(engine), std::ref(readersRunning), std::ref(snapshotReads));
    }

    std::vector<std::thread> producers;
    std::vector<ProducerStats> producerStats(producerCount);
    producers.reserve(producerCount);

    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t p = 0; p < producerCount; ++p) {
        producers.emplace_back([&, p] {
            std::mt19937_64 rng(0xC0FFEEULL + p);
            const std::uint64_t begin = p * perProducer;
            const std::uint64_t end = std::min<std::uint64_t>(begin + perProducer, totalOrders);
            auto& stats = producerStats[p];

            for (std::uint64_t i = begin; i < end; ++i) {
                const auto before = std::chrono::steady_clock::now();
                engine.submit(makeRandomEvent(rng, p, i - begin, producerCount, perProducer));
                const auto after = std::chrono::steady_clock::now();
                const auto latency = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
                stats.maxSubmitLatencyNs = std::max(stats.maxSubmitLatencyNs, latency);
                stats.totalSubmitLatencyNs += latency;
                ++stats.sent;
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    engine.stop();
    readersRunning.store(false, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }

    const auto finish = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(finish - start).count();

    std::uint64_t sent = 0;
    std::uint64_t maxLatency = 0;
    long double totalLatency = 0.0;
    for (const auto& stats : producerStats) {
        sent += stats.sent;
        maxLatency = std::max(maxLatency, stats.maxSubmitLatencyNs);
        totalLatency += stats.totalSubmitLatencyNs;
    }

    const auto& stats = engine.stats();
    auto snapshot = engine.snapshot();

    std::cout << "Processed events: " << stats.processed.load(std::memory_order_relaxed) << "\n";
    std::cout << "Submitted events: " << sent << "\n";
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << (sent / seconds) << " orders/sec\n";
    std::cout << "Elapsed: " << seconds << " sec\n";
    std::cout << "Average submit latency: " << std::setprecision(2) << (sent == 0 ? 0.0 : static_cast<double>(totalLatency / sent)) << " ns\n";
    std::cout << "Max submit latency: " << maxLatency << " ns\n";
    std::cout << "Filled quantity: " << stats.filledQuantity.load(std::memory_order_relaxed) << "\n";
    std::cout << "Live orders: " << engine.liveOrders() << "\n";
    std::cout << "Snapshots published: " << stats.publishedSnapshots.load(std::memory_order_relaxed) << "\n";
    std::cout << "Snapshot reads: " << snapshotReads.load(std::memory_order_relaxed) << "\n";
    std::cout << "Book invariant check: " << (engine.verify() ? "PASS" : "FAIL") << "\n";
    if (snapshot) {
        std::cout << "Current buy orders: " << snapshot->bidSummary.orderCount
                  << " orders, quantity " << snapshot->bidSummary.totalQuantity << "\n";
        std::cout << "Current sell orders: " << snapshot->askSummary.orderCount
                  << " orders, quantity " << snapshot->askSummary.totalQuantity << "\n";
        std::cout << "Best bid: " << snapshot->bestBid() << "\n";
        std::cout << "Best ask: " << snapshot->bestAsk() << "\n";
        std::cout << "Spread: " << snapshot->spread() << "\n";
        std::cout << "Mid: " << snapshot->midPrice() << "\n";
    }

    return engine.verify() ? 0 : 1;
}
