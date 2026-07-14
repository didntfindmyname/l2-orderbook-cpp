#pragma once

#include "ConcurrentQueue.hpp"
#include "OrderBook.hpp"
#include "Snapshot.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

struct alignas(64) EngineStats {
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> processed{0};
    std::atomic<std::uint64_t> added{0};
    std::atomic<std::uint64_t> canceled{0};
    std::atomic<std::uint64_t> modified{0};
    std::atomic<std::uint64_t> marketOrders{0};
    std::atomic<std::uint64_t> filledQuantity{0};
    std::atomic<std::uint64_t> publishedSnapshots{0};
};

class MatchingEngine {
public:
    explicit MatchingEngine(
        std::size_t reserveOrders = 1'000'000,
        std::size_t snapshotLevels = 10,
        std::uint64_t snapshotEvery = 65'536,
        std::size_t latencySamples = 0)
        : book_(reserveOrders), snapshotLevels_(snapshotLevels), snapshotEvery_(snapshotEvery) {
        processLatencyNs_.reserve(latencySamples);
    }

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    ~MatchingEngine() {
        stop();
    }

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        matcher_ = std::thread(&MatchingEngine::run, this);
    }

    void submit(OrderEvent event) {
        stats_.submitted.fetch_add(1, std::memory_order_relaxed);
        queue_.push(std::move(event));
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return;
        }
        queue_.push(OrderEvent::shutdown());
        queue_.notifyAll();
        if (matcher_.joinable()) {
            matcher_.join();
        }
    }

    [[nodiscard]] std::shared_ptr<const BookSnapshot> snapshot() const noexcept {
        return snapshots_.load();
    }

    [[nodiscard]] const EngineStats& stats() const noexcept {
        return stats_;
    }

    [[nodiscard]] bool verify() const {
        return verified_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t liveOrders() const noexcept {
        return liveOrders_.load(std::memory_order_acquire);
    }

    [[nodiscard]] const std::vector<std::uint64_t>& processLatenciesNs() const noexcept {
        return processLatencyNs_;
    }

private:
    void run() {
        std::uint64_t sequence = 0;
        publish(sequence);

        OrderEvent event;
        while (queue_.waitPop(event, running_)) {
            if (event.eventType == EventType::Shutdown) {
                break;
            }

            const auto before = std::chrono::steady_clock::now();
            ++sequence;
            process(event, sequence);
            const auto after = std::chrono::steady_clock::now();
            processLatencyNs_.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
            stats_.processed.fetch_add(1, std::memory_order_relaxed);

            if ((sequence % snapshotEvery_) == 0) {
                publish(sequence);
            }
        }

        while (queue_.tryPop(event)) {
            if (event.eventType != EventType::Shutdown) {
                const auto before = std::chrono::steady_clock::now();
                ++sequence;
                process(event, sequence);
                const auto after = std::chrono::steady_clock::now();
                processLatencyNs_.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
                stats_.processed.fetch_add(1, std::memory_order_relaxed);
            }
        }

        publish(sequence);
        liveOrders_.store(book_.liveOrders(), std::memory_order_release);
        verified_.store(book_.verifyInvariants(), std::memory_order_release);
    }

    void process(const OrderEvent& event, std::uint64_t sequence) {
        OrderBook::ExecutionReport report;
        switch (event.eventType) {
        case EventType::NewOrder:
            if (event.orderType == OrderType::Market) {
                report = book_.executeMarketOrder(event.side, event.quantity);
                stats_.marketOrders.fetch_add(1, std::memory_order_relaxed);
            } else {
                report = book_.addOrder(event.id, event.side, event.price, event.quantity, sequence);
                stats_.added.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case EventType::ModifyOrder:
            report = book_.modifyOrder(event.id, event.price, event.quantity, sequence);
            stats_.modified.fetch_add(1, std::memory_order_relaxed);
            break;
        case EventType::CancelOrder:
            if (book_.cancelOrder(event.id)) {
                stats_.canceled.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case EventType::Shutdown:
            break;
        }

        stats_.filledQuantity.fetch_add(report.filled, std::memory_order_relaxed);
    }

    void publish(std::uint64_t sequence) {
        snapshots_.publish(book_.makeSnapshot(snapshotLevels_, sequence));
        stats_.publishedSnapshots.fetch_add(1, std::memory_order_relaxed);
    }

    OrderBook book_;
    MPSCQueue<OrderEvent> queue_;
    SnapshotPublisher snapshots_;
    std::thread matcher_;
    std::atomic<bool> running_{false};
    EngineStats stats_;
    std::size_t snapshotLevels_;
    std::uint64_t snapshotEvery_;
    std::atomic<bool> verified_{false};
    std::atomic<std::size_t> liveOrders_{0};
    std::vector<std::uint64_t> processLatencyNs_;
};
