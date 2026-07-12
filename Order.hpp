#pragma once

#include <cstdint>
#include <string>

using OrderID = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;

enum class Side : std::uint8_t {
    Buy,
    Sell
};

enum class OrderType : std::uint8_t {
    Limit,
    Market
};

enum class EventType : std::uint8_t {
    NewOrder,
    ModifyOrder,
    CancelOrder,
    Shutdown
};

struct Order {
    OrderID id{};
    Side side{};
    Price price{};
    Quantity quantity{};
    std::uint64_t sequence{};
};

struct OrderEvent {
    EventType eventType{EventType::NewOrder};
    OrderType orderType{OrderType::Limit};
    OrderID id{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};

    static OrderEvent shutdown() noexcept {
        OrderEvent event;
        event.eventType = EventType::Shutdown;
        return event;
    }
};

inline const char* toString(Side side) noexcept {
    return side == Side::Buy ? "BUY" : "SELL";
}

