#pragma once

#include "Order.hpp"
#include "PriceLevel.hpp"
#include "Snapshot.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

class OrderBook {
public:
    struct Trade {
        OrderID aggressorOrderId{};
        OrderID restingOrderId{};
        Side aggressorSide{};
        Price price{};
        Quantity quantity{};
    };

    struct ExecutionReport {
        Quantity requested{};
        Quantity filled{};
        Quantity resting{};
        std::vector<Trade> trades;

        [[nodiscard]] bool fullyFilled() const noexcept {
            return requested == filled;
        }
    };

    struct OrderHandle {
        Side side{};
        Price price{};
        std::list<Order*>::iterator iterator{};
    };

    explicit OrderBook(std::size_t reserveOrders = 1'000'000) {
        orderIndex_.reserve(reserveOrders);
        orderStorage_.reserve(reserveOrders);
    }

    ExecutionReport addOrder(OrderID id, Side side, Price price, Quantity quantity, std::uint64_t sequence) {
        if (quantity == 0 || id == 0 || orderIndex_.contains(id)) {
            return {};
        }

        ExecutionReport report{quantity, 0, 0};
        Quantity remaining = quantity;
        if (side == Side::Buy) {
            remaining = matchBuy(id, price, remaining, report);
        } else {
            remaining = matchSell(id, price, remaining, report);
        }

        if (remaining > 0) {
            addRestingOrder(id, side, price, remaining, sequence);
            report.resting = remaining;
        }

        return report;
    }

    ExecutionReport executeMarketOrder(Side side, Quantity quantity, OrderID id = 0) {
        if (quantity == 0) {
            return {};
        }

        ExecutionReport report{quantity, 0, 0};
        if (side == Side::Buy) {
            matchBuy(id, std::numeric_limits<Price>::max(), quantity, report);
        } else {
            matchSell(id, std::numeric_limits<Price>::min(), quantity, report);
        }
        return report;
    }

    bool cancelOrder(OrderID id) {
        auto indexIt = orderIndex_.find(id);
        if (indexIt == orderIndex_.end()) {
            return false;
        }

        Order* order = indexIt->second;
        auto handleIt = handles_.find(id);
        if (handleIt == handles_.end()) {
            return false;
        }

        removeRestingOrder(order, handleIt->second);
        return true;
    }

    ExecutionReport modifyOrder(OrderID id, Price newPrice, Quantity newQuantity, std::uint64_t sequence) {
        auto indexIt = orderIndex_.find(id);
        if (indexIt == orderIndex_.end() || newQuantity == 0) {
            if (newQuantity == 0) {
                cancelOrder(id);
            }
            return {};
        }

        Order* order = indexIt->second;
        auto handleIt = handles_.find(id);
        if (handleIt == handles_.end()) {
            return {};
        }

        OrderHandle handle = handleIt->second;
        if (newPrice == order->price && newQuantity <= order->quantity) {
            Quantity delta = order->quantity - newQuantity;
            order->quantity = newQuantity;
            levelFor(handle.side, handle.price).aggregateQuantity -= delta;
            subtractSideQuantity(handle.side, delta);
            return ExecutionReport{newQuantity, 0, newQuantity};
        }

        Side side = order->side;
        cancelOrder(id);
        return addOrder(id, side, newPrice, newQuantity, sequence);
    }

    [[nodiscard]] std::optional<Price> bestBid() const noexcept {
        if (bids_.empty()) {
            return std::nullopt;
        }
        return bids_.begin()->first;
    }

    [[nodiscard]] std::optional<Price> bestAsk() const noexcept {
        if (asks_.empty()) {
            return std::nullopt;
        }
        return asks_.begin()->first;
    }

    [[nodiscard]] std::optional<Price> spread() const noexcept {
        if (bids_.empty() || asks_.empty()) {
            return std::nullopt;
        }
        return asks_.begin()->first - bids_.begin()->first;
    }

    [[nodiscard]] std::optional<double> midPrice() const noexcept {
        if (bids_.empty() || asks_.empty()) {
            return std::nullopt;
        }
        return (static_cast<double>(bids_.begin()->first) + asks_.begin()->first) / 2.0;
    }

    [[nodiscard]] std::shared_ptr<const BookSnapshot> makeSnapshot(std::size_t levels, std::uint64_t sequence) const {
        auto snapshot = std::make_shared<BookSnapshot>();
        snapshot->sequence = sequence;
        snapshot->bidSummary = SideSummary{bidOrderCount_, bidQuantity_};
        snapshot->askSummary = SideSummary{askOrderCount_, askQuantity_};
        snapshot->bids.reserve(std::min(levels, bids_.size()));
        snapshot->asks.reserve(std::min(levels, asks_.size()));

        for (auto it = bids_.begin(); it != bids_.end() && snapshot->bids.size() < levels; ++it) {
            snapshot->bids.push_back({it->first, it->second.aggregateQuantity, it->second.fifo.size()});
        }
        for (auto it = asks_.begin(); it != asks_.end() && snapshot->asks.size() < levels; ++it) {
            snapshot->asks.push_back({it->first, it->second.aggregateQuantity, it->second.fifo.size()});
        }

        return snapshot;
    }

    [[nodiscard]] std::vector<DepthLevel> getDepth(Side side, std::size_t levels) const {
        std::vector<DepthLevel> depth;
        depth.reserve(levels);
        if (side == Side::Buy) {
            for (auto it = bids_.begin(); it != bids_.end() && depth.size() < levels; ++it) {
                depth.push_back({it->first, it->second.aggregateQuantity, it->second.fifo.size()});
            }
        } else {
            for (auto it = asks_.begin(); it != asks_.end() && depth.size() < levels; ++it) {
                depth.push_back({it->first, it->second.aggregateQuantity, it->second.fifo.size()});
            }
        }
        return depth;
    }

    void printBook(std::size_t levels = 10) const {
        auto asks = getDepth(Side::Sell, levels);
        auto bids = getDepth(Side::Buy, levels);

        std::cout << "ASKS\n";
        for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
            std::cout << std::setw(8) << it->price << " x " << it->quantity << " (" << it->orderCount << ")\n";
        }
        std::cout << "BIDS\n";
        for (const auto& bid : bids) {
            std::cout << std::setw(8) << bid.price << " x " << bid.quantity << " (" << bid.orderCount << ")\n";
        }
    }

    [[nodiscard]] bool verifyInvariants() const {
        Quantity indexedOrders = 0;
        Quantity computedBidQuantity = 0;
        Quantity computedAskQuantity = 0;
        std::size_t computedBidOrders = 0;
        std::size_t computedAskOrders = 0;
        for (const auto& [price, level] : bids_) {
            if (level.empty() || level.price != price) {
                return false;
            }
            Quantity sum = 0;
            for (const Order* order : level.fifo) {
                if (order->side != Side::Buy || order->price != price || !orderIndex_.contains(order->id)) {
                    return false;
                }
                sum += order->quantity;
                ++indexedOrders;
                ++computedBidOrders;
            }
            if (sum != level.aggregateQuantity) {
                return false;
            }
            computedBidQuantity += sum;
        }
        for (const auto& [price, level] : asks_) {
            if (level.empty() || level.price != price) {
                return false;
            }
            Quantity sum = 0;
            for (const Order* order : level.fifo) {
                if (order->side != Side::Sell || order->price != price || !orderIndex_.contains(order->id)) {
                    return false;
                }
                sum += order->quantity;
                ++indexedOrders;
                ++computedAskOrders;
            }
            if (sum != level.aggregateQuantity) {
                return false;
            }
            computedAskQuantity += sum;
        }
        return indexedOrders == orderIndex_.size()
            && handles_.size() == orderIndex_.size()
            && computedBidOrders == bidOrderCount_
            && computedAskOrders == askOrderCount_
            && computedBidQuantity == bidQuantity_
            && computedAskQuantity == askQuantity_;
    }

    [[nodiscard]] std::size_t liveOrders() const noexcept {
        return orderIndex_.size();
    }

    [[nodiscard]] SideSummary buySummary() const noexcept {
        return SideSummary{bidOrderCount_, bidQuantity_};
    }

    [[nodiscard]] SideSummary sellSummary() const noexcept {
        return SideSummary{askOrderCount_, askQuantity_};
    }

private:
    using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskLevels = std::map<Price, PriceLevel, std::less<Price>>;

    Quantity matchBuy(OrderID aggressorOrderId, Price limitPrice, Quantity quantity, ExecutionReport& report) {
        while (quantity > 0 && !asks_.empty()) {
            auto levelIt = asks_.begin();
            if (levelIt->first > limitPrice) {
                break;
            }
            quantity = consumeLevel(aggressorOrderId, Side::Buy, levelIt, asks_, quantity, report);
        }
        return quantity;
    }

    Quantity matchSell(OrderID aggressorOrderId, Price limitPrice, Quantity quantity, ExecutionReport& report) {
        while (quantity > 0 && !bids_.empty()) {
            auto levelIt = bids_.begin();
            if (levelIt->first < limitPrice) {
                break;
            }
            quantity = consumeLevel(aggressorOrderId, Side::Sell, levelIt, bids_, quantity, report);
        }
        return quantity;
    }

    template <typename Levels>
    Quantity consumeLevel(
        OrderID aggressorOrderId,
        Side aggressorSide,
        typename Levels::iterator levelIt,
        Levels& levels,
        Quantity quantity,
        ExecutionReport& report) {
        PriceLevel& level = levelIt->second;
        while (quantity > 0 && !level.fifo.empty()) {
            Order* resting = level.fifo.front();
            Quantity traded = std::min(quantity, resting->quantity);
            report.trades.push_back(Trade{aggressorOrderId, resting->id, aggressorSide, resting->price, traded});
            resting->quantity -= traded;
            level.aggregateQuantity -= traded;
            subtractSideQuantity(resting->side, traded);
            quantity -= traded;
            report.filled += traded;

            if (resting->quantity == 0) {
                removeFilledFront(resting, level);
            }
        }

        if (level.empty()) {
            levels.erase(levelIt);
        }

        return quantity;
    }

    void addRestingOrder(OrderID id, Side side, Price price, Quantity quantity, std::uint64_t sequence) {
        auto order = std::make_unique<Order>(Order{id, side, price, quantity, sequence});
        Order* raw = order.get();
        orderStorage_.emplace(id, std::move(order));

        PriceLevel& level = levelFor(side, price);
        level.fifo.push_back(raw);
        auto iterator = std::prev(level.fifo.end());
        level.aggregateQuantity += quantity;
        orderIndex_.emplace(id, raw);
        handles_.emplace(id, OrderHandle{side, price, iterator});
        addSideOrder(side, quantity);
    }

    PriceLevel& levelFor(Side side, Price price) {
        if (side == Side::Buy) {
            auto [it, inserted] = bids_.try_emplace(price, price);
            return it->second;
        }

        auto [it, inserted] = asks_.try_emplace(price, price);
        return it->second;
    }

    void removeRestingOrder(Order* order, const OrderHandle& handle) {
        if (handle.side == Side::Buy) {
            auto levelIt = bids_.find(handle.price);
            if (levelIt != bids_.end()) {
                eraseFromLevel(order, handle, levelIt, bids_);
            }
        } else {
            auto levelIt = asks_.find(handle.price);
            if (levelIt != asks_.end()) {
                eraseFromLevel(order, handle, levelIt, asks_);
            }
        }
    }

    template <typename Levels>
    void eraseFromLevel(Order* order, const OrderHandle& handle, typename Levels::iterator levelIt, Levels& levels) {
        PriceLevel& level = levelIt->second;
        level.aggregateQuantity -= order->quantity;
        removeSideOrder(order->side, order->quantity);
        level.fifo.erase(handle.iterator);
        orderIndex_.erase(order->id);
        handles_.erase(order->id);
        orderStorage_.erase(order->id);
        if (level.empty()) {
            levels.erase(levelIt);
        }
    }

    void removeFilledFront(Order* order, PriceLevel& level) {
        level.fifo.pop_front();
        removeSideOrder(order->side, 0);
        orderIndex_.erase(order->id);
        handles_.erase(order->id);
        orderStorage_.erase(order->id);
    }

    void addSideOrder(Side side, Quantity quantity) noexcept {
        if (side == Side::Buy) {
            ++bidOrderCount_;
            bidQuantity_ += quantity;
        } else {
            ++askOrderCount_;
            askQuantity_ += quantity;
        }
    }

    void removeSideOrder(Side side, Quantity remainingQuantity) noexcept {
        if (side == Side::Buy) {
            --bidOrderCount_;
            bidQuantity_ -= remainingQuantity;
        } else {
            --askOrderCount_;
            askQuantity_ -= remainingQuantity;
        }
    }

    void subtractSideQuantity(Side side, Quantity quantity) noexcept {
        if (side == Side::Buy) {
            bidQuantity_ -= quantity;
        } else {
            askQuantity_ -= quantity;
        }
    }

    BidLevels bids_;
    AskLevels asks_;
    std::size_t bidOrderCount_{0};
    std::size_t askOrderCount_{0};
    Quantity bidQuantity_{0};
    Quantity askQuantity_{0};

    // Required fast ID lookup. It is non-owning; orderStorage_ owns the objects.
    std::unordered_map<OrderID, Order*> orderIndex_;
    std::unordered_map<OrderID, OrderHandle> handles_;
    std::unordered_map<OrderID, std::unique_ptr<Order>> orderStorage_;
};
