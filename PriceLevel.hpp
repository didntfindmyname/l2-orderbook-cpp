#pragma once

#include "Order.hpp"

#include <list>

struct PriceLevel {
    Price price{};
    Quantity aggregateQuantity{};
    std::list<Order*> fifo;

    explicit PriceLevel(Price levelPrice = 0) noexcept : price(levelPrice) {}

    [[nodiscard]] bool empty() const noexcept {
        return fifo.empty();
    }
};

