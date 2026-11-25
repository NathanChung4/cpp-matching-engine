// The fast order book: one symbol, price-time priority.
//
// Data structures (README "Design note" explains the why):
//   - price levels indexed directly by price tick, one array per side, so finding the
//     level for a price is one array access;
//   - a bitmap per side with one bit per non-empty level, so the next best price after a
//     level empties is found with a count-leading/trailing-zeros scan of 64 levels at a time;
//   - each level is an intrusive doubly linked FIFO of order nodes, stored in one pool
//     vector with a free list (no allocation per order once warm);
//   - an open-addressing map from order id to node index, so cancel is O(1): look up the
//     node, unlink it from its level, free it.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "me/events.hpp"
#include "me/flat_map.hpp"
#include "me/types.hpp"

namespace me {

template <class Sink>
class Book {
public:
    explicit Book(Sink& sink, std::size_t expected_orders = 1 << 16)
        : sink_(sink), ids_(expected_orders) {
        nodes_.reserve(std::min<std::size_t>(expected_orders, 1 << 20));
        for (SideBook& s : sides_) {
            s.levels.assign(static_cast<std::size_t>(kMaxPrice) + 1, Level{});
            s.bits.assign(kWords, 0);
        }
        bids().best = kNoBid;
        asks().best = kNoAsk;
    }

    // A new order of any type. Emits its events to the sink.
    void submit(const NewOrder& o) {
        if (const Reason r = validate(o); r != Reason::None) {
            sink_(Event::rejected(o.id, r));
            return;
        }
        execute(o);
    }

    // Cancels a resting order.
    void cancel(OrderId id) {
        const std::int32_t n = ids_.find(id);
        if (n < 0) {
            sink_(Event::rejected(id, Reason::UnknownId));
            return;
        }
        const Node& node = nodes_[static_cast<std::size_t>(n)];
        sink_(Event::cancelled(id, node.side, node.price, node.qty));
        unlink(n);
        ids_.set(id, IdMap::kGone);
    }

    // Cancel-replace: the resting order leaves the book (Cancelled) and re-enters with the
    // same id, the new price and quantity and a new place in the queue (DECISIONS.md D4).
    void modify(OrderId id, Price price, Qty qty) {
        const std::int32_t n = ids_.find(id);
        if (n < 0) {
            sink_(Event::rejected(id, Reason::UnknownId));
            return;
        }
        if (qty == 0 || qty > kMaxQty) {
            sink_(Event::rejected(id, Reason::BadQty));
            return;
        }
        if (price < kMinPrice || price > kMaxPrice) {
            sink_(Event::rejected(id, Reason::PriceOutOfRange));
            return;
        }
        const Node& node = nodes_[static_cast<std::size_t>(n)];
        const Side s = node.side;
        sink_(Event::cancelled(id, s, node.price, node.qty));
        unlink(n);
        execute({id, s, OrderType::Limit, price, qty});
    }

    std::optional<Price> best_bid() const {
        return bids().best == kNoBid ? std::nullopt : std::optional<Price>(bids().best);
    }
    std::optional<Price> best_ask() const {
        return asks().best == kNoAsk ? std::nullopt : std::optional<Price>(asks().best);
    }
    std::size_t resting() const { return resting_; }

    // Every resting order, bids best first then asks best first, FIFO within a level.
    Snapshot snapshot() const {
        Snapshot out;
        out.reserve(resting_);
        for (Side s : {Side::Buy, Side::Sell}) {
            const SideBook& sb = side(s);
            for (Price p = sb.best; p != none(s); p = next_level(sb, s, p, false)) {
                for (std::int32_t i = sb.levels[static_cast<std::size_t>(p)].head; i != -1;
                     i = nodes_[static_cast<std::size_t>(i)].next) {
                    const Node& n = nodes_[static_cast<std::size_t>(i)];
                    out.push_back({s, p, n.id, n.qty});
                }
            }
        }
        return out;
    }

    // Total resting quantity at one price on one side (0 if none).
    std::uint64_t level_qty(Side s, Price p) const {
        if (p < kMinPrice || p > kMaxPrice) return 0;
        return side(s).levels[static_cast<std::size_t>(p)].qty;
    }

private:
    static constexpr std::size_t kWords = (static_cast<std::size_t>(kMaxPrice) + 1 + 63) / 64;
    static constexpr Price kNoBid = 0;              // below every valid price
    static constexpr Price kNoAsk = kMaxPrice + 1;  // above every valid price

    struct Node {
        OrderId id;
        Price price;
        Qty qty;
        Side side;
        std::int32_t prev;
        std::int32_t next;
    };
    struct Level {
        std::int32_t head = -1;
        std::int32_t tail = -1;
        std::uint64_t qty = 0;
    };
    struct SideBook {
        std::vector<Level> levels;
        std::vector<std::uint64_t> bits;
        Price best = 0;
    };

    SideBook& side(Side s) { return sides_[static_cast<std::size_t>(s)]; }
    const SideBook& side(Side s) const { return sides_[static_cast<std::size_t>(s)]; }
    SideBook& bids() { return side(Side::Buy); }
    SideBook& asks() { return side(Side::Sell); }
    const SideBook& bids() const { return side(Side::Buy); }
    const SideBook& asks() const { return side(Side::Sell); }
    static constexpr Price none(Side s) { return s == Side::Buy ? kNoBid : kNoAsk; }

    Reason validate(const NewOrder& o) const {
        if (ids_.find(o.id) != IdMap::kAbsent) return Reason::DuplicateId;
        if (o.qty == 0 || o.qty > kMaxQty) return Reason::BadQty;
        if (o.type != OrderType::Market && (o.price < kMinPrice || o.price > kMaxPrice))
            return Reason::PriceOutOfRange;
        if (o.type == OrderType::FOK && !fillable(o.side, o.price, o.qty)) return Reason::FokNotFillable;
        return Reason::None;
    }

    // Is there at least `qty` on the opposite side at prices that cross `limit`? Walks the
    // levels from the best price and stops as soon as it has enough.
    bool fillable(Side s, Price limit, Qty qty) const {
        const Side os = opposite(s);
        const SideBook& book = side(os);
        std::uint64_t sum = 0;
        for (Price p = book.best; p != none(os) && crosses(s, limit, p); p = next_level(book, os, p, false)) {
            sum += book.levels[static_cast<std::size_t>(p)].qty;
            if (sum >= qty) return true;
        }
        return false;
    }

    // Does a resting price on the opposite side cross the incoming order's limit?
    static bool crosses(Side incoming, Price limit, Price resting) {
        return incoming == Side::Buy ? resting <= limit : resting >= limit;
    }

    void execute(const NewOrder& o) {
        const bool market = o.type == OrderType::Market;
        const Price limit = market ? (o.side == Side::Buy ? kMaxPrice : kMinPrice) : o.price;
        Qty left = match(o.id, o.side, limit, o.qty);
        if (left == 0) {
            ids_.set(o.id, IdMap::kGone);
            return;
        }
        if (market || o.type == OrderType::IOC) {
            sink_(Event::expired(o.id, o.side, market ? 0 : o.price, left));
            ids_.set(o.id, IdMap::kGone);
            return;
        }
        rest(o.id, o.side, o.price, left);
    }

    // Takes liquidity from the opposite side; returns the quantity left.
    Qty match(OrderId taker, Side s, Price limit, Qty left) {
        const Side os = opposite(s);
        SideBook& book = side(os);
        while (left > 0 && book.best != none(os) && crosses(s, limit, book.best)) {
            const Price p = book.best;
            Level& lv = book.levels[static_cast<std::size_t>(p)];
            std::int32_t i = lv.head;
            while (i != -1 && left > 0) {
                Node& n = nodes_[static_cast<std::size_t>(i)];
                const Qty f = std::min(left, n.qty);
                sink_(Event::trade(taker, n.id, s, p, f));
                left -= f;
                n.qty -= f;
                lv.qty -= f;
                if (n.qty == 0) {
                    const std::int32_t nx = n.next;
                    ids_.set(n.id, IdMap::kGone);
                    pop_front(book, lv, p, i);
                    i = nx;
                }
            }
        }
        return left;
    }

    void rest(OrderId id, Side s, Price p, Qty q) {
        std::int32_t i;
        if (!free_.empty()) {
            i = free_.back();
            free_.pop_back();
        } else {
            i = static_cast<std::int32_t>(nodes_.size());
            nodes_.push_back({});
        }
        SideBook& book = side(s);
        Level& lv = book.levels[static_cast<std::size_t>(p)];
        nodes_[static_cast<std::size_t>(i)] = {id, p, q, s, lv.tail, -1};
        if (lv.tail != -1)
            nodes_[static_cast<std::size_t>(lv.tail)].next = i;
        else {
            lv.head = i;
            set_bit(book, p);
            if (s == Side::Buy ? p > book.best : p < book.best) book.best = p;
        }
        lv.tail = i;
        lv.qty += q;
        ++resting_;
        ids_.set(id, i);
        sink_(Event::rest(id, s, p, q));
    }

    // Removes the head node of a level after it filled.
    void pop_front(SideBook& book, Level& lv, Price p, std::int32_t i) {
        const std::int32_t nx = nodes_[static_cast<std::size_t>(i)].next;
        lv.head = nx;
        if (nx != -1)
            nodes_[static_cast<std::size_t>(nx)].prev = -1;
        else
            level_emptied(book, lv, p);
        free_.push_back(i);
        --resting_;
    }

    // Unlinks any node (cancel). O(1).
    void unlink(std::int32_t i) {
        Node& n = nodes_[static_cast<std::size_t>(i)];
        SideBook& book = side(n.side);
        Level& lv = book.levels[static_cast<std::size_t>(n.price)];
        if (n.prev != -1)
            nodes_[static_cast<std::size_t>(n.prev)].next = n.next;
        else
            lv.head = n.next;
        if (n.next != -1)
            nodes_[static_cast<std::size_t>(n.next)].prev = n.prev;
        else
            lv.tail = n.prev;
        lv.qty -= n.qty;
        if (lv.head == -1) level_emptied(book, lv, n.price);
        free_.push_back(i);
        --resting_;
    }

    void level_emptied(SideBook& book, Level& lv, Price p) {
        lv.head = lv.tail = -1;
        lv.qty = 0;
        clear_bit(book, p);
        const Side s = &book == &bids() ? Side::Buy : Side::Sell;
        if (book.best == p) book.best = next_level(book, s, p, true);
    }

    static void set_bit(SideBook& b, Price p) {
        b.bits[static_cast<std::size_t>(p) >> 6] |= 1ULL << (p & 63);
    }
    static void clear_bit(SideBook& b, Price p) {
        b.bits[static_cast<std::size_t>(p) >> 6] &= ~(1ULL << (p & 63));
    }

    // The next non-empty level past p, going away from the spread (down for bids, up for
    // asks), or none(s). `p` itself is skipped.
    static Price next_level(const SideBook& b, Side s, Price p, bool /*after_clear*/) {
        if (s == Side::Buy) {
            if (p <= kMinPrice) return kNoBid;
            Price q = p - 1;
            std::size_t w = static_cast<std::size_t>(q) >> 6;
            std::uint64_t word = b.bits[w] & (~0ULL >> (63 - (q & 63)));
            while (true) {
                if (word != 0)
                    return static_cast<Price>((w << 6) + 63 - static_cast<std::size_t>(std::countl_zero(word)));
                if (w == 0) return kNoBid;
                word = b.bits[--w];
            }
        }
        if (p >= kMaxPrice) return kNoAsk;
        Price q = p + 1;
        std::size_t w = static_cast<std::size_t>(q) >> 6;
        std::uint64_t word = b.bits[w] & (~0ULL << (q & 63));
        while (true) {
            if (word != 0)
                return static_cast<Price>((w << 6) + static_cast<std::size_t>(std::countr_zero(word)));
            if (++w == kWords) return kNoAsk;
            word = b.bits[w];
        }
    }

    Sink& sink_;
    std::array<SideBook, 2> sides_;
    std::vector<Node> nodes_;
    std::vector<std::int32_t> free_;
    IdMap ids_;
    std::size_t resting_ = 0;
};

}  // namespace me
