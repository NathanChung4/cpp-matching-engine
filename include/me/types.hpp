// Core types shared by the fast engine (book.hpp) and the reference matcher (reference.hpp).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace me {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // integer ticks of $0.01: 100.05 is 10005
using Qty = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market, IOC, FOK };

// The price band the fast book indexes directly (DECISIONS.md D3). Both matchers reject
// prices outside it with the same reason, so they stay comparable.
inline constexpr Price kMinPrice = 1;
inline constexpr Price kMaxPrice = 131071;
inline constexpr Qty kMaxQty = 1'000'000'000;

struct NewOrder {
    OrderId id;
    Side side;
    OrderType type;
    Price price;  // ignored for market orders
    Qty qty;
};

enum class Reason : std::uint8_t {
    None,
    DuplicateId,
    UnknownId,
    BadQty,
    PriceOutOfRange,
    FokNotFillable,
};

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

constexpr std::string_view side_text(Side s) { return s == Side::Buy ? "buy" : "sell"; }

constexpr std::string_view reason_text(Reason r) {
    switch (r) {
        case Reason::None: return "none";
        case Reason::DuplicateId: return "duplicate order id";
        case Reason::UnknownId: return "unknown order id";
        case Reason::BadQty: return "bad quantity";
        case Reason::PriceOutOfRange: return "price out of range";
        case Reason::FokNotFillable: return "FOK not fillable";
    }
    return "?";
}

// 10005 -> "100.05"
inline std::string price_text(Price p) {
    const bool neg = p < 0;
    const auto a = static_cast<std::uint64_t>(neg ? -p : p);
    std::string cents = std::to_string(a % 100);
    if (cents.size() < 2) cents.insert(0, "0");
    return (neg ? "-" : "") + std::to_string(a / 100) + "." + cents;
}

}  // namespace me
