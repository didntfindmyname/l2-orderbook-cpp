#pragma once

#include "Order.hpp"

#include <atomic>
#include <memory>
#include <vector>

struct DepthLevel {
    Price price{};
    Quantity quantity{};
    std::size_t orderCount{};
};

struct SideSummary {
    std::size_t orderCount{};
    Quantity totalQuantity{};
};

struct BookSnapshot {
    std::uint64_t sequence{};
    SideSummary bidSummary;
    SideSummary askSummary;
    std::vector<DepthLevel> bids;
    std::vector<DepthLevel> asks;

    [[nodiscard]] Price bestBid() const noexcept {
        return bids.empty() ? 0 : bids.front().price;
    }

    [[nodiscard]] Price bestAsk() const noexcept {
        return asks.empty() ? 0 : asks.front().price;
    }

    [[nodiscard]] Price spread() const noexcept {
        return bids.empty() || asks.empty() ? 0 : asks.front().price - bids.front().price;
    }

    [[nodiscard]] double midPrice() const noexcept {
        return bids.empty() || asks.empty() ? 0.0 : (static_cast<double>(bids.front().price) + asks.front().price) / 2.0;
    }
};

class SnapshotPublisher {
public:
    void publish(std::shared_ptr<const BookSnapshot> snapshot) noexcept {
        // Release publishes all writes used to build the immutable snapshot before readers see the pointer.
        current_.store(std::move(snapshot), std::memory_order_release);
    }

    [[nodiscard]] std::shared_ptr<const BookSnapshot> load() const noexcept {
        // Acquire pairs with publish() so readers observe a fully initialized BookSnapshot.
        return current_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::shared_ptr<const BookSnapshot>> current_{nullptr};
};
