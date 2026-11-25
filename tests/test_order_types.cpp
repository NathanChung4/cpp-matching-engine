// order-types (S1): modify (cancel-replace that loses queue priority), IOC and FOK.
#include <vector>

#include "me/book.hpp"
#include "testlib.hpp"

using namespace me;
using E = Event;
using Events = std::vector<Event>;

namespace {

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
    Events ioc(OrderId id, Side s, Price p, Qty q) {
        book.submit({id, s, OrderType::IOC, p, q});
        return take();
    }
    Events fok(OrderId id, Side s, Price p, Qty q) {
        book.submit({id, s, OrderType::FOK, p, q});
        return take();
    }
    Events modify(OrderId id, Price p, Qty q) {
        book.modify(id, p, q);
        return take();
    }
};

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

}  // namespace

// ---- modify ----

TEST("modify: cancel-replace reports the cancel, then the order rests again") {
    Fixture f;
    f.limit(1, S, 10005, 10);
    CHECK_EQ(f.modify(1, 10007, 4), (Events{E::cancelled(1, S, 10005, 10), E::rest(1, S, 10007, 4)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{S, 10007, 1, 4}}));
    CHECK_EQ(f.book.level_qty(S, 10005), std::uint64_t{0});
    CHECK_EQ(f.book.best_ask(), std::optional<Price>(10007));
}

TEST("modify: the order loses queue priority even at the same price") {
    Fixture f;
    f.limit(1, B, 10000, 5);
    f.limit(2, B, 10000, 5);
    f.modify(1, 10000, 5);  // same price and qty: still goes to the back
    CHECK_EQ(f.book.snapshot(), (Snapshot{{B, 10000, 2, 5}, {B, 10000, 1, 5}}));
    CHECK_EQ(f.limit(3, S, 10000, 5), (Events{E::trade(3, 2, S, 10000, 5)}));
}

TEST("modify: a quantity decrease also loses priority (DECISIONS.md D4)") {
    Fixture f;
    f.limit(1, S, 10000, 9);
    f.limit(2, S, 10000, 9);
    f.modify(1, 10000, 3);
    CHECK_EQ(f.limit(3, B, 10000, 10), (Events{E::trade(3, 2, B, 10000, 9), E::trade(3, 1, B, 10000, 1)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{S, 10000, 1, 2}}));
}

TEST("modify: a new price that crosses trades at once, then rests the remainder") {
    Fixture f;
    f.limit(1, S, 10010, 4);
    f.limit(2, B, 10000, 10);
    CHECK_EQ(f.modify(2, 10010, 10),
             (Events{E::cancelled(2, B, 10000, 10), E::trade(2, 1, B, 10010, 4), E::rest(2, B, 10010, 6)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{B, 10010, 2, 6}}));
}

TEST("modify: a partly filled order is replaced from its remaining quantity") {
    Fixture f;
    f.limit(1, S, 10000, 10);
    f.limit(2, B, 10000, 3);
    CHECK_EQ(f.modify(1, 10001, 20), (Events{E::cancelled(1, S, 10000, 7), E::rest(1, S, 10001, 20)}));
}

TEST("modify: unknown, filled or cancelled ids are rejected") {
    Fixture f;
    CHECK_EQ(f.modify(9, 10000, 1), (Events{E::rejected(9, Reason::UnknownId)}));
    f.limit(1, S, 10000, 5);
    f.limit(2, B, 10000, 5);
    CHECK_EQ(f.modify(1, 10000, 1), (Events{E::rejected(1, Reason::UnknownId)}));
    f.limit(3, S, 10000, 5);
    f.book.cancel(3);
    f.take();
    CHECK_EQ(f.modify(3, 10000, 1), (Events{E::rejected(3, Reason::UnknownId)}));
}

TEST("modify: bad quantity or price is rejected and the order stays as it was") {
    Fixture f;
    f.limit(1, S, 10000, 5);
    CHECK_EQ(f.modify(1, 10000, 0), (Events{E::rejected(1, Reason::BadQty)}));
    CHECK_EQ(f.modify(1, kMaxPrice + 1, 5), (Events{E::rejected(1, Reason::PriceOutOfRange)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{S, 10000, 1, 5}}));
    CHECK_EQ(f.book.resting(), std::size_t{1});
}

TEST("modify keeps the id: a later cancel by that id works, a new order with it is a duplicate") {
    Fixture f;
    f.limit(1, B, 10000, 5);
    f.modify(1, 9990, 5);
    CHECK_EQ(f.limit(1, B, 9000, 1), (Events{E::rejected(1, Reason::DuplicateId)}));
    f.book.cancel(1);
    CHECK_EQ(f.take(), (Events{E::cancelled(1, B, 9990, 5)}));
}

// ---- IOC ----

TEST("IOC: fills what crosses at or better than its limit, the rest expires, nothing rests") {
    Fixture f;
    f.limit(1, S, 10000, 3);
    f.limit(2, S, 10001, 3);
    f.limit(3, S, 10002, 3);
    CHECK_EQ(f.ioc(4, B, 10001, 10),
             (Events{E::trade(4, 1, B, 10000, 3), E::trade(4, 2, B, 10001, 3), E::expired(4, B, 10001, 4)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{S, 10002, 3, 3}}));
    CHECK(!f.book.best_bid().has_value());
}

TEST("IOC: no cross means it expires in full; full fill means no expiry event") {
    Fixture f;
    f.limit(1, B, 9990, 5);
    CHECK_EQ(f.ioc(2, S, 9995, 5), (Events{E::expired(2, S, 9995, 5)}));
    CHECK_EQ(f.ioc(3, S, 9990, 5), (Events{E::trade(3, 1, S, 9990, 5)}));
    CHECK_EQ(f.ioc(4, B, 10000, 1), (Events{E::expired(4, B, 10000, 1)}));  // empty book
    CHECK(f.book.snapshot().empty());
}

TEST("IOC: its id is used up; cancel of it is rejected") {
    Fixture f;
    f.ioc(1, B, 10000, 5);
    CHECK_EQ(f.limit(1, B, 10000, 5), (Events{E::rejected(1, Reason::DuplicateId)}));
    f.book.cancel(1);
    CHECK_EQ(f.take(), (Events{E::rejected(1, Reason::UnknownId)}));
}

TEST("IOC: validated like a limit order") {
    Fixture f;
    CHECK_EQ(f.ioc(1, B, 0, 5), (Events{E::rejected(1, Reason::PriceOutOfRange)}));
    CHECK_EQ(f.ioc(2, B, 10000, 0), (Events{E::rejected(2, Reason::BadQty)}));
}

// ---- FOK ----

TEST("FOK: enough crossing quantity across levels executes in full") {
    Fixture f;
    f.limit(1, S, 10000, 3);
    f.limit(2, S, 10001, 3);
    f.limit(3, S, 10002, 3);
    CHECK_EQ(f.fok(4, B, 10001, 6), (Events{E::trade(4, 1, B, 10000, 3), E::trade(4, 2, B, 10001, 3)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{S, 10002, 3, 3}}));
}

TEST("FOK: not enough within the limit is rejected with no trades and the book untouched") {
    Fixture f;
    f.limit(1, S, 10000, 3);
    f.limit(2, S, 10001, 3);
    f.limit(3, S, 10002, 3);  // beyond the limit: does not count
    CHECK_EQ(f.fok(4, B, 10001, 7), (Events{E::rejected(4, Reason::FokNotFillable)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{S, 10000, 1, 3}, {S, 10001, 2, 3}, {S, 10002, 3, 3}}));
    CHECK_EQ(f.fok(5, S, 9000, 1), (Events{E::rejected(5, Reason::FokNotFillable)}));  // no bids
}

TEST("FOK: exactly the available quantity fills; a partial maker keeps its remainder") {
    Fixture f;
    f.limit(1, B, 10000, 4);
    f.limit(2, B, 9999, 4);
    CHECK_EQ(f.fok(3, S, 9999, 8), (Events{E::trade(3, 1, S, 10000, 4), E::trade(3, 2, S, 9999, 4)}));
    f.limit(4, B, 9998, 10);
    CHECK_EQ(f.fok(5, S, 9998, 6), (Events{E::trade(5, 4, S, 9998, 6)}));
    CHECK_EQ(f.book.snapshot(), (Snapshot{{B, 9998, 4, 4}}));
}

TEST("FOK: a rejected FOK does not use up its id (DECISIONS.md D7)") {
    Fixture f;
    CHECK_EQ(f.fok(1, B, 10000, 5), (Events{E::rejected(1, Reason::FokNotFillable)}));
    f.limit(2, S, 10000, 5);
    CHECK_EQ(f.fok(1, B, 10000, 5), (Events{E::trade(1, 2, B, 10000, 5)}));
    CHECK_EQ(f.fok(1, B, 10000, 5), (Events{E::rejected(1, Reason::DuplicateId)}));
}

TEST("FOK: other rejects come first") {
    Fixture f;
    CHECK_EQ(f.fok(1, B, 10000, 0), (Events{E::rejected(1, Reason::BadQty)}));
    CHECK_EQ(f.fok(2, B, kMaxPrice + 1, 5), (Events{E::rejected(2, Reason::PriceOutOfRange)}));
}
