// core-matching (M1, M3): limit, market and cancel orders by price-time priority, every
// trade reported as an event, and the matching edge cases.
#include <vector>

#include "me/book.hpp"
#include "testlib.hpp"

using namespace me;
using E = Event;
using Events = std::vector<Event>;

namespace {

// A book plus helpers that return exactly the events one command produced.
struct Fixture {
    VectorSink sink;
    Book<VectorSink> book{sink};

    Events take() {
        Events out;
        out.swap(sink.events);
        return out;
    }
    Events limit(OrderId id, Side s, Price p, Qty q) {
        book.submit({id, s, OrderType::Limit, p, q});
        return take();
    }
    Events market(OrderId id, Side s, Qty q) {
        book.submit({id, s, OrderType::Market, 0, q});
        return take();
    }
    Events cancel(OrderId id) {
        book.cancel(id);
        return take();
    }
};

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

}  // namespace

TEST("empty book: no best prices, empty snapshot") {
    Fixture f;
    CHECK(!f.book.best_bid().has_value());
    CHECK(!f.book.best_ask().has_value());
    CHECK(f.book.snapshot().empty());
    CHECK_EQ(f.book.resting(), std::size_t{0});
}

TEST("empty book: a market order expires in full with no trades") {
    Fixture f;
    CHECK_EQ(f.market(1, B, 50), (Events{E::expired(1, B, 0, 50)}));
    CHECK_EQ(f.market(2, S, 7), (Events{E::expired(2, S, 0, 7)}));
    CHECK(f.book.snapshot().empty());
}

TEST("empty book: cancel is rejected as unknown order id") {
    Fixture f;
    CHECK_EQ(f.cancel(42), (Events{E::rejected(42, Reason::UnknownId)}));
}

TEST("a limit order that does not cross rests and is reported") {
    Fixture f;
    CHECK_EQ(f.limit(1, S, 10005, 10), (Events{E::rest(1, S, 10005, 10)}));
    CHECK_EQ(f.limit(2, B, 10000, 3), (Events{E::rest(2, B, 10000, 3)}));
    CHECK_EQ(f.book.best_ask(), std::optional<Price>(10005));
    CHECK_EQ(f.book.best_bid(), std::optional<Price>(10000));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{B, 10000, 2, 3}, {S, 10005, 1, 10}}));
}

TEST("price priority: the best price fills first, whatever the arrival order") {
    Fixture f;
    f.limit(1, S, 10100, 5);
    f.limit(2, S, 10000, 5);
    f.limit(3, S, 10200, 5);
    CHECK_EQ(f.limit(4, B, 10200, 1), (Events{E::trade(4, 2, B, 10000, 1)}));
    f.limit(5, B, 9800, 5);
    f.limit(6, B, 9900, 5);
    f.limit(7, B, 9700, 5);
    CHECK_EQ(f.limit(8, S, 9700, 2), (Events{E::trade(8, 6, S, 9900, 2)}));
}

TEST("time priority: at one price, the earliest order fills first") {
    Fixture f;
    f.limit(1, S, 10000, 5);
    f.limit(2, S, 10000, 5);
    f.limit(3, S, 10000, 5);
    CHECK_EQ(f.limit(4, B, 10000, 7), (Events{E::trade(4, 1, B, 10000, 5), E::trade(4, 2, B, 10000, 2)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{S, 10000, 2, 3}, {S, 10000, 3, 5}}));
}

TEST("trades print at the resting (maker) order's price") {
    Fixture f;
    f.limit(1, S, 10000, 5);
    CHECK_EQ(f.limit(2, B, 10500, 5), (Events{E::trade(2, 1, B, 10000, 5)}));
    f.limit(3, B, 9000, 5);
    CHECK_EQ(f.limit(4, S, 8000, 5), (Events{E::trade(4, 3, S, 9000, 5)}));
}

TEST("partial fill: the maker keeps its remainder and its place") {
    Fixture f;
    f.limit(1, S, 10000, 10);
    f.limit(2, S, 10000, 10);
    CHECK_EQ(f.limit(3, B, 10000, 4), (Events{E::trade(3, 1, B, 10000, 4)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{S, 10000, 1, 6}, {S, 10000, 2, 10}}));
    CHECK_EQ(f.book.level_qty(S, 10000), std::uint64_t{16});
}

TEST("partial fill: the taker's remainder rests at its limit") {
    Fixture f;
    f.limit(1, S, 10000, 6);
    CHECK_EQ(f.limit(2, B, 10000, 10), (Events{E::trade(2, 1, B, 10000, 6), E::rest(2, B, 10000, 4)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{B, 10000, 2, 4}}));
    CHECK(!f.book.best_ask().has_value());
}

TEST("sweep: one order takes several levels in price order, then rests") {
    Fixture f;
    f.limit(1, S, 10000, 5);
    f.limit(2, S, 10001, 5);
    f.limit(3, S, 10001, 1);
    f.limit(4, S, 10002, 5);
    CHECK_EQ(f.limit(5, B, 10001, 13),
             (Events{E::trade(5, 1, B, 10000, 5), E::trade(5, 2, B, 10001, 5), E::trade(5, 3, B, 10001, 1),
                     E::rest(5, B, 10001, 2)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{B, 10001, 5, 2}, {S, 10002, 4, 5}}));
    CHECK_EQ(f.book.best_ask(), std::optional<Price>(10002));
    CHECK_EQ(f.book.best_bid(), std::optional<Price>(10001));
}

TEST("sweep (sell side): an incoming sell takes bid levels best first and stops at its limit") {
    Fixture f;
    f.limit(1, B, 10002, 5);
    f.limit(2, B, 10001, 5);
    f.limit(3, B, 10001, 1);
    f.limit(4, B, 10000, 5);
    CHECK_EQ(f.limit(5, S, 10001, 13),
             (Events{E::trade(5, 1, S, 10002, 5), E::trade(5, 2, S, 10001, 5), E::trade(5, 3, S, 10001, 1),
                     E::rest(5, S, 10001, 2)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{B, 10000, 4, 5}, {S, 10001, 5, 2}}));
    CHECK_EQ(f.book.best_bid(), std::optional<Price>(10000));
    CHECK_EQ(f.book.best_ask(), std::optional<Price>(10001));
}

TEST("sweep: a market buy takes several ask levels, the rest expires") {
    Fixture f;
    f.limit(1, S, 10000, 2);
    f.limit(2, S, 10005, 3);
    f.limit(3, S, 10050, 4);
    CHECK_EQ(f.market(4, B, 15),
             (Events{E::trade(4, 1, B, 10000, 2), E::trade(4, 2, B, 10005, 3), E::trade(4, 3, B, 10050, 4),
                     E::expired(4, B, 0, 6)}));
    CHECK(!f.book.best_ask().has_value());
    CHECK(f.book.snapshot().empty());
}

TEST("sweep: a market order takes every level it needs, the rest expires") {
    Fixture f;
    f.limit(1, B, 9999, 5);
    f.limit(2, B, 9990, 5);
    CHECK_EQ(f.market(3, S, 12),
             (Events{E::trade(3, 1, S, 9999, 5), E::trade(3, 2, S, 9990, 5), E::expired(3, S, 0, 2)}));
    CHECK(f.book.snapshot().empty());
    CHECK(!f.book.best_bid().has_value());
}

TEST("sweep: best prices are found across bitmap words and far apart") {
    Fixture f;
    f.limit(1, S, kMaxPrice, 1);
    f.limit(2, S, 64 * 7 + 3, 1);
    f.limit(3, S, 64 * 7 + 1, 1);
    f.limit(4, B, kMinPrice, 1);
    f.limit(5, B, 64 * 2 + 63, 1);
    f.limit(6, B, 64 * 3, 1);
    CHECK_EQ(f.book.best_ask(), std::optional<Price>(64 * 7 + 1));
    CHECK_EQ(f.book.best_bid(), std::optional<Price>(64 * 3));
    CHECK_EQ(f.limit(7, B, kMaxPrice, 3),
             (Events{E::trade(7, 3, B, 64 * 7 + 1, 1), E::trade(7, 2, B, 64 * 7 + 3, 1),
                     E::trade(7, 1, B, kMaxPrice, 1)}));
    CHECK(!f.book.best_ask().has_value());
    CHECK_EQ(f.limit(8, S, kMinPrice, 3),
             (Events{E::trade(8, 6, S, 64 * 3, 1), E::trade(8, 5, S, 64 * 2 + 63, 1),
                     E::trade(8, 4, S, kMinPrice, 1)}));
    CHECK(f.book.snapshot().empty());
}

TEST("cancel of a resting order removes it and reports what was left") {
    Fixture f;
    f.limit(1, B, 10000, 5);
    f.limit(2, B, 10000, 6);
    f.limit(3, B, 10000, 7);
    f.limit(9, S, 10000, 1);  // fills 1 of #1
    f.take();
    CHECK_EQ(f.cancel(1), (Events{E::cancelled(1, B, 10000, 4)}));
    CHECK_EQ(f.cancel(2), (Events{E::cancelled(2, B, 10000, 6)}));  // #2 became the head when #1 left
    CHECK_EQ(f.book.snapshot(), (Snapshot{{B, 10000, 3, 7}}));
    CHECK_EQ(f.book.level_qty(B, 10000), std::uint64_t{7});
    CHECK_EQ(f.cancel(3), (Events{E::cancelled(3, B, 10000, 7)}));
    CHECK(!f.book.best_bid().has_value());
    CHECK(f.book.snapshot().empty());
}

TEST("cancel from the middle and the tail keeps FIFO order of the rest") {
    Fixture f;
    for (OrderId id = 1; id <= 5; ++id) f.limit(id, S, 10010, static_cast<Qty>(id));
    f.cancel(3);
    f.cancel(5);
    CHECK_EQ(f.book.snapshot(), (Snapshot{{S, 10010, 1, 1}, {S, 10010, 2, 2}, {S, 10010, 4, 4}}));
    f.limit(6, S, 10010, 6);
    CHECK_EQ(f.limit(7, B, 10010, 13),
             (Events{E::trade(7, 1, B, 10010, 1), E::trade(7, 2, B, 10010, 2), E::trade(7, 4, B, 10010, 4),
                     E::trade(7, 6, B, 10010, 6)}));
}

TEST("cancel of an already-filled order is rejected as unknown order id") {
    Fixture f;
    f.limit(1, S, 10000, 5);
    f.limit(2, B, 10000, 5);
    CHECK_EQ(f.cancel(1), (Events{E::rejected(1, Reason::UnknownId)}));
    CHECK_EQ(f.cancel(2), (Events{E::rejected(2, Reason::UnknownId)}));  // the taker filled in full too
}

TEST("cancel twice: the second is rejected") {
    Fixture f;
    f.limit(1, S, 10000, 5);
    CHECK_EQ(f.cancel(1), (Events{E::cancelled(1, S, 10000, 5)}));
    CHECK_EQ(f.cancel(1), (Events{E::rejected(1, Reason::UnknownId)}));
}

TEST("cancel of an expired market order is rejected") {
    Fixture f;
    f.market(1, B, 5);
    CHECK_EQ(f.cancel(1), (Events{E::rejected(1, Reason::UnknownId)}));
}

TEST("rejects: duplicate id among live and past orders") {
    Fixture f;
    f.limit(1, S, 10000, 5);
    CHECK_EQ(f.limit(1, S, 10001, 5), (Events{E::rejected(1, Reason::DuplicateId)}));
    f.limit(2, B, 10000, 5);  // fills #1 and #2
    CHECK_EQ(f.limit(1, S, 10001, 5), (Events{E::rejected(1, Reason::DuplicateId)}));
    CHECK_EQ(f.market(2, S, 5), (Events{E::rejected(2, Reason::DuplicateId)}));
    f.cancel(3);  // a rejected cancel does not use the id
    CHECK_EQ(f.limit(3, S, 10001, 5), (Events{E::rest(3, S, 10001, 5)}));
}

TEST("rejects: bad quantity and price out of range; a rejected id can be reused") {
    Fixture f;
    CHECK_EQ(f.limit(1, B, 10000, 0), (Events{E::rejected(1, Reason::BadQty)}));
    CHECK_EQ(f.limit(2, B, 10000, kMaxQty + 1), (Events{E::rejected(2, Reason::BadQty)}));
    CHECK_EQ(f.market(3, B, 0), (Events{E::rejected(3, Reason::BadQty)}));
    CHECK_EQ(f.limit(4, B, 0, 5), (Events{E::rejected(4, Reason::PriceOutOfRange)}));
    CHECK_EQ(f.limit(5, S, kMaxPrice + 1, 5), (Events{E::rejected(5, Reason::PriceOutOfRange)}));
    CHECK_EQ(f.limit(6, S, -3, 5), (Events{E::rejected(6, Reason::PriceOutOfRange)}));
    CHECK(f.book.snapshot().empty());
    CHECK_EQ(f.limit(1, B, 10000, 3), (Events{E::rest(1, B, 10000, 3)}));
    CHECK_EQ(f.limit(7, B, kMinPrice, kMaxQty), (Events{E::rest(7, B, kMinPrice, kMaxQty)}));
    CHECK_EQ(f.limit(8, S, kMaxPrice, 1), (Events{E::rest(8, S, kMaxPrice, 1)}));
}

TEST("market orders ignore their price field") {
    Fixture f;
    f.limit(1, S, 10000, 5);
    f.book.submit({2, B, OrderType::Market, -999, 5});
    CHECK_EQ(f.take(), (Events{E::trade(2, 1, B, 10000, 5)}));
}

TEST("every trade is reported: a scripted session's full event stream") {
    Fixture f;
    // Straight to the book (the helpers would take the events one command at a time).
    f.book.submit({10, S, OrderType::Limit, 10003, 4});
    f.book.submit({11, S, OrderType::Limit, 10002, 3});
    f.book.submit({12, B, OrderType::Limit, 10000, 6});
    f.book.submit({13, S, OrderType::Limit, 10002, 2});
    f.book.submit({14, B, OrderType::Market, 0, 4});
    f.book.cancel(13);
    f.book.submit({15, S, OrderType::Limit, 9999, 8});
    f.book.cancel(12);
    const Events all = f.take();
    CHECK_EQ(all, (Events{E::rest(10, S, 10003, 4), E::rest(11, S, 10002, 3), E::rest(12, B, 10000, 6),
                          E::rest(13, S, 10002, 2), E::trade(14, 11, B, 10002, 3), E::trade(14, 13, B, 10002, 1),
                          E::cancelled(13, S, 10002, 1), E::trade(15, 12, S, 10000, 6), E::rest(15, S, 9999, 2),
                          E::rejected(12, Reason::UnknownId)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{S, 9999, 15, 2}, {S, 10003, 10, 4}}));
}

TEST("level quantity stays the sum of its orders through fills and cancels") {
    Fixture f;
    f.limit(1, B, 10000, 10);
    f.limit(2, B, 10000, 20);
    f.limit(3, B, 10000, 30);
    CHECK_EQ(f.book.level_qty(B, 10000), std::uint64_t{60});
    f.limit(4, S, 10000, 15);
    CHECK_EQ(f.book.level_qty(B, 10000), std::uint64_t{45});
    f.cancel(3);
    CHECK_EQ(f.book.level_qty(B, 10000), std::uint64_t{15});
    CHECK_EQ(f.book.resting(), std::size_t{1});
}

TEST("many orders: node slots are reused after fills and cancels") {
    Fixture f;
    for (int round = 0; round < 50; ++round) {
        for (OrderId k = 0; k < 200; ++k) f.limit(1 + round * 1000 + k, S, 10000 + static_cast<Price>(k % 7), 1);
        f.market(1 + round * 1000 + 500, B, 150);
        // The market order took the 150 best by price-time; cancel every id: the filled
        // ones are rejected, the other 50 leave the book.
        f.take();
        for (OrderId k = 0; k < 200; ++k) f.book.cancel(1 + round * 1000 + k);
        const Events ev = f.take();
        std::size_t cancelled = 0;
        for (const Event& e : ev) cancelled += e.type == EventType::Cancelled ? 1 : 0;
        CHECK_EQ(cancelled, std::size_t{50});
        CHECK_EQ(f.book.resting(), std::size_t{0});
    }
    CHECK(f.book.snapshot().empty());
}
