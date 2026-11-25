// The reference matcher: slow on purpose, and simple enough to check by reading.
//
// The whole book is one vector of resting orders. Every step scans all of it:
//   - the best maker is the resting order on the other side with the best price, and among
//     equal prices the lowest arrival number (price-time priority, written as a comparison);
//   - cancel is a linear search;
//   - FOK sums every crossing order's quantity before deciding.
// There are no levels, no bitmaps, no hash maps and no linked lists: none of the fast
// book's data structures. The differential test runs both on the same orders and demands
// the same events and the same final book.
#pragma once

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

#include "me/events.hpp"
#include "me/types.hpp"

namespace me {

class ReferenceBook {
public:
    explicit ReferenceBook(std::vector<Event>& out) : out_(out) {}

    void submit(const NewOrder& o) {
        if (used_.count(o.id) != 0) return reject(o.id, Reason::DuplicateId);
        if (o.qty == 0 || o.qty > kMaxQty) return reject(o.id, Reason::BadQty);
        if (o.type != OrderType::Market && (o.price < kMinPrice || o.price > kMaxPrice))
            return reject(o.id, Reason::PriceOutOfRange);
        if (o.type == OrderType::FOK && available(o.side, o.price) < o.qty)
            return reject(o.id, Reason::FokNotFillable);
        used_.insert(o.id);
        enter(o.id, o.side, o.type, o.price, o.qty);
    }

    void cancel(OrderId id) {
        const auto it = find(id);
        if (it == book_.end()) return reject(id, Reason::UnknownId);
        out_.push_back(Event::cancelled(it->id, it->side, it->price, it->qty));
        book_.erase(it);
    }

    void modify(OrderId id, Price price, Qty qty) {
        const auto it = find(id);
        if (it == book_.end()) return reject(id, Reason::UnknownId);
        if (qty == 0 || qty > kMaxQty) return reject(id, Reason::BadQty);
        if (price < kMinPrice || price > kMaxPrice) return reject(id, Reason::PriceOutOfRange);
        const Side side = it->side;
        out_.push_back(Event::cancelled(it->id, it->side, it->price, it->qty));
        book_.erase(it);
        enter(id, side, OrderType::Limit, price, qty);  // a new arrival number: priority lost
    }

    // Bids best first, then asks best first; within a price, arrival order.
    Snapshot snapshot() const {
        std::vector<Order> sorted = book_;
        std::sort(sorted.begin(), sorted.end(), [](const Order& a, const Order& b) {
            if (a.side != b.side) return a.side == Side::Buy;
            if (a.price != b.price) return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
            return a.seq < b.seq;
        });
        Snapshot s;
        for (const Order& o : sorted) s.push_back({o.side, o.price, o.id, o.qty});
        return s;
    }

    std::size_t resting() const { return book_.size(); }

private:
    struct Order {
        OrderId id;
        Side side;
        Price price;
        Qty qty;
        std::uint64_t seq;  // arrival number: lower is earlier
    };

    void reject(OrderId id, Reason r) { out_.push_back(Event::rejected(id, r)); }

    std::vector<Order>::iterator find(OrderId id) {
        return std::find_if(book_.begin(), book_.end(), [&](const Order& o) { return o.id == id; });
    }

    static bool crosses(Side taker, OrderType type, Price limit, Price maker_price) {
        if (type == OrderType::Market) return true;
        return taker == Side::Buy ? maker_price <= limit : maker_price >= limit;
    }

    // Is maker a better than maker b for an incoming order on `taker` side?
    static bool better(Side taker, const Order& a, const Order& b) {
        if (a.price != b.price) return taker == Side::Buy ? a.price < b.price : a.price > b.price;
        return a.seq < b.seq;
    }

    // Quantity on the other side at prices that cross `limit`.
    std::uint64_t available(Side taker, Price limit) const {
        std::uint64_t sum = 0;
        for (const Order& m : book_)
            if (m.side != taker && crosses(taker, OrderType::Limit, limit, m.price)) sum += m.qty;
        return sum;
    }

    void enter(OrderId id, Side side, OrderType type, Price price, Qty qty) {
        const std::uint64_t seq = next_seq_++;
        while (qty > 0) {
            // Scan the whole book for the best maker.
            auto best = book_.end();
            for (auto it = book_.begin(); it != book_.end(); ++it) {
                if (it->side == side || !crosses(side, type, price, it->price)) continue;
                if (best == book_.end() || better(side, *it, *best)) best = it;
            }
            if (best == book_.end()) break;
            const Qty f = std::min(qty, best->qty);
            out_.push_back(Event::trade(id, best->id, side, best->price, f));
            qty -= f;
            best->qty -= f;
            if (best->qty == 0) book_.erase(best);
        }
        if (qty == 0) return;
        if (type == OrderType::Market || type == OrderType::IOC) {
            out_.push_back(Event::expired(id, side, type == OrderType::Market ? 0 : price, qty));
            return;
        }
        book_.push_back({id, side, price, qty, seq});
        out_.push_back(Event::rest(id, side, price, qty));
    }

    std::vector<Event>& out_;
    std::vector<Order> book_;
    std::set<OrderId> used_;  // ids of accepted orders (DECISIONS.md D7)
    std::uint64_t next_seq_ = 0;
};

}  // namespace me
