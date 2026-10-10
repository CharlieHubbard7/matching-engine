#pragma once
#include <cstdint>

namespace me {

using OrderId = uint64_t;
using Price = uint32_t;  // integer ticks
using Qty = uint32_t;
using SymbolId = uint16_t;

enum class Side : uint8_t { Buy = 0, Sell = 1 };
enum class Tif : uint8_t { Gtc = 0, Ioc = 1 };
enum class Reject : uint8_t { DuplicateId = 1, UnknownOrder, BadSymbol, BadQty, BookFull };

constexpr Side opposite(Side s) noexcept { return Side(uint8_t(s) ^ 1); }

// Opaque tag the caller attaches to an order (the gateway uses the session
// id) and gets back when that order trades.
using Owner = uint32_t;
constexpr Owner kNoOwner = UINT32_MAX;

struct Fill {
    OrderId resting;
    OrderId incoming;
    Owner resting_owner;
    Price price;  // always the resting order's price
    Qty qty;
    SymbolId symbol;
};

}  // namespace me
