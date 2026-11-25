// The engine's output: one Event per thing that happened, in order. Both matchers emit
// exactly the same events for the same input, which is what the differential test checks.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "me/types.hpp"

namespace me {

enum class EventType : std::uint8_t { Trade, Rest, Cancelled, Expired, Rejected };

struct Event {
    EventType type{};
    OrderId id{};     // Trade: the taker. Others: the order the event is about.
    OrderId maker{};  // Trade only: the resting order that was hit.
    Side side{};      // Trade: the taker's side. Rest/Cancelled/Expired: the order's side.
    Price price{};    // Trade: the maker's price. Rest/Cancelled: the order's price.
                      // Expired: the limit price (0 for a market order).
    Qty qty{};        // Trade: traded qty. Rest/Cancelled/Expired: the qty concerned.
    Reason reason{};  // Rejected only.

    bool operator==(const Event&) const = default;

    static Event trade(OrderId taker, OrderId maker, Side taker_side, Price p, Qty q) {
        return {EventType::Trade, taker, maker, taker_side, p, q, Reason::None};
    }
    static Event rest(OrderId id, Side s, Price p, Qty q) {
        return {EventType::Rest, id, 0, s, p, q, Reason::None};
    }
    static Event cancelled(OrderId id, Side s, Price p, Qty q) {
        return {EventType::Cancelled, id, 0, s, p, q, Reason::None};
    }
    static Event expired(OrderId id, Side s, Price p, Qty q) {
        return {EventType::Expired, id, 0, s, p, q, Reason::None};
    }
    static Event rejected(OrderId id, Reason r) {
        return {EventType::Rejected, id, 0, Side::Buy, 0, 0, r};
    }
};

// One line, for test failures and the difftest's mismatch report.
inline std::string to_string(const Event& e) {
    const std::string id = "#" + std::to_string(e.id);
    switch (e.type) {
        case EventType::Trade:
            return "TRADE " + std::to_string(e.qty) + " @ " + price_text(e.price) + "  taker " + id +
                   "  maker #" + std::to_string(e.maker);
        case EventType::Rest:
            return "REST " + id + " " + std::string(side_text(e.side)) + " " + std::to_string(e.qty) +
                   " @ " + price_text(e.price);
        case EventType::Cancelled:
            return "CANCELLED " + id + " " + std::to_string(e.qty) + " left @ " + price_text(e.price);
        case EventType::Expired:
            return "EXPIRED " + id + " " + std::to_string(e.qty) + " unfilled";
        case EventType::Rejected:
            return "REJECTED " + id + ": " + std::string(reason_text(e.reason));
    }
    return "?";
}

// A sink that keeps every event (tests, the ladder, the difftest).
struct VectorSink {
    std::vector<Event> events;
    void operator()(const Event& e) { events.push_back(e); }
};

// A sink that keeps only a running hash and counts (the benchmark): the same seed must
// always give the same checksum.
struct ChecksumSink {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    std::uint64_t trades = 0;
    std::uint64_t events = 0;

    static std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h * 0x100000001b3ULL;
    }
    void operator()(const Event& e) {
        ++events;
        if (e.type == EventType::Trade) ++trades;
        std::uint64_t h = mix(hash, static_cast<std::uint64_t>(e.type));
        h = mix(h, e.id);
        h = mix(h, e.maker);
        h = mix(h, static_cast<std::uint64_t>(e.side));
        h = mix(h, static_cast<std::uint64_t>(e.price));
        h = mix(h, e.qty);
        hash = mix(h, static_cast<std::uint64_t>(e.reason));
    }
};

// One resting order in a book snapshot.
struct Resting {
    Side side{};
    Price price{};
    OrderId id{};
    Qty qty{};
    bool operator==(const Resting&) const = default;
};

inline std::string to_string(const Resting& r) {
    return std::string(side_text(r.side)) + " " + price_text(r.price) + " #" + std::to_string(r.id) + ":" +
           std::to_string(r.qty);
}

// Every resting order: bids best (highest) first, then asks best (lowest) first; within a
// price, arrival order.
using Snapshot = std::vector<Resting>;

}  // namespace me
