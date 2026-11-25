// differential (M2, P3): the fast book and the slow reference matcher must agree on every
// event and on the final book, over at least 2,000,000 seeded random orders.
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "me/difftest.hpp"
#include "testlib.hpp"

using namespace me;
using E = Event;
using Events = std::vector<Event>;

namespace {

#ifdef ME_SANITIZE
constexpr std::size_t kOrdersPerSeed = 100'000;  // the sanitizer build is much slower
#else
constexpr std::size_t kOrdersPerSeed = 500'000;  // 4 seeds: 2,000,000 orders
#endif
constexpr std::array<std::uint64_t, 4> kSeeds{1, 2, 3, 20261125};

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

void apply_to(Book<VectorSink>& book, const Command& c) {
    switch (c.kind) {
        case CommandKind::New: book.submit(c.order); break;
        case CommandKind::Cancel: book.cancel(c.target); break;
        case CommandKind::Modify: book.modify(c.target, c.price, c.qty); break;
    }
}
void apply_to(ReferenceBook& book, const Command& c) {
    switch (c.kind) {
        case CommandKind::New: book.submit(c.order); break;
        case CommandKind::Cancel: book.cancel(c.target); break;
        case CommandKind::Modify: book.modify(c.target, c.price, c.qty); break;
    }
}

// A deliberately wrong engine for the seam: the real fast book, but from command
// `bad_at` on it changes the first trade's quantity (an event bug), or, with
// `book_only`, it reports one resting order less in its snapshot (a book bug with the
// same events).
struct WrongBook {
    VectorSink sink;
    Book<VectorSink> book{sink};
    std::vector<Event>& out;
    std::size_t bad_at;
    bool book_only;
    std::size_t n = 0;

    WrongBook(std::vector<Event>& o, std::size_t at, bool only_book) : out(o), bad_at(at), book_only(only_book) {}
    void after() {
        if (!book_only && n++ >= bad_at)
            for (Event& e : sink.events)
                if (e.type == EventType::Trade) {
                    e.qty += 1;
                    break;
                }
        out.insert(out.end(), sink.events.begin(), sink.events.end());
        sink.events.clear();
    }
    void submit(const NewOrder& o) { book.submit(o); after(); }
    void cancel(OrderId id) { book.cancel(id); after(); }
    void modify(OrderId id, Price p, Qty q) { book.modify(id, p, q); after(); }
    Snapshot snapshot() const {
        Snapshot s = book.snapshot();
        if (book_only && !s.empty()) s.pop_back();
        return s;
    }
    std::size_t resting() const { return book.resting(); }
};

}  // namespace

TEST("the reference matcher on its own follows the rules (a hand-checked session)") {
    Events out;
    ReferenceBook r(out);
    r.submit({10, S, OrderType::Limit, 10003, 4});
    r.submit({11, S, OrderType::Limit, 10002, 3});
    r.submit({12, B, OrderType::Limit, 10000, 6});
    r.submit({13, S, OrderType::Limit, 10002, 2});
    r.submit({14, B, OrderType::Market, 0, 4});
    r.cancel(13);
    r.submit({15, S, OrderType::Limit, 9999, 8});
    r.cancel(12);
    r.submit({16, B, OrderType::IOC, 10003, 9});
    r.submit({17, S, OrderType::Limit, 10001, 5});
    r.submit({18, B, OrderType::FOK, 10001, 6});
    r.submit({19, B, OrderType::FOK, 10001, 5});
    r.modify(17, 10002, 5);
    r.modify(17, 10002, 0);
    r.submit({20, S, OrderType::Limit, 10002, 1});
    r.modify(20, 10002, 1);
    CHECK_EQ(out, (Events{E::rest(10, S, 10003, 4), E::rest(11, S, 10002, 3), E::rest(12, B, 10000, 6),
                          E::rest(13, S, 10002, 2), E::trade(14, 11, B, 10002, 3), E::trade(14, 13, B, 10002, 1),
                          E::cancelled(13, S, 10002, 1), E::trade(15, 12, S, 10000, 6), E::rest(15, S, 9999, 2),
                          E::rejected(12, Reason::UnknownId), E::trade(16, 15, B, 9999, 2),
                          E::trade(16, 10, B, 10003, 4), E::expired(16, B, 10003, 3), E::rest(17, S, 10001, 5),
                          E::rejected(18, Reason::FokNotFillable), E::trade(19, 17, B, 10001, 5),
                          E::rejected(17, Reason::UnknownId), E::rejected(17, Reason::UnknownId),
                          E::rest(20, S, 10002, 1), E::cancelled(20, S, 10002, 1), E::rest(20, S, 10002, 1)}));
    CHECK(r.snapshot() == (Snapshot{{S, 10002, 20, 1}}));
}

TEST("the comparison functions catch a changed, missing, extra or reordered event") {
    const Events good{E::rest(1, S, 10000, 5), E::trade(2, 1, B, 10000, 2), E::trade(2, 3, B, 10000, 1)};
    CHECK(!first_difference(good, good, 0, 0).has_value());
    Events qty = good;
    qty[1].qty = 3;
    CHECK(first_difference(qty, good, 0, 0).has_value());
    Events maker = good;
    maker[2].maker = 4;
    CHECK(first_difference(maker, good, 0, 0).has_value());
    Events missing(good.begin(), good.end() - 1);
    const auto m = first_difference(missing, good, 7, 100);
    REQUIRE(m.has_value());
    CHECK_EQ(m->command, std::size_t{7});
    CHECK_EQ(m->event, std::size_t{102});
    CHECK_EQ(m->engine, std::string("(none)"));
    CHECK_EQ(m->reference, to_string(good[2]));
    Events extra = good;
    extra.push_back(E::expired(2, B, 10000, 1));
    CHECK(first_difference(extra, good, 0, 0).has_value());
    Events swapped{good[0], good[2], good[1]};
    CHECK(first_difference(swapped, good, 0, 0).has_value());
    CHECK(snapshot_difference(Snapshot{{S, 10000, 1, 5}}, Snapshot{{S, 10000, 1, 4}}, 0).has_value());
    CHECK(snapshot_difference(Snapshot{{S, 10000, 1, 5}, {S, 10000, 2, 5}},
                              Snapshot{{S, 10000, 2, 5}, {S, 10000, 1, 5}}, 0)
              .has_value());
    CHECK(!snapshot_difference(Snapshot{{B, 9999, 1, 5}}, Snapshot{{B, 9999, 1, 5}}, 0).has_value());
}

TEST("compare_run reports an injected event bug at the right command and event") {
    const auto flow = make_flow(5, 20'000);
    // Where does the first trade at or after command 1000 happen, and what is its global event index?
    std::size_t want_cmd = 0, want_event = 0;
    Event want_ref{};
    {
        VectorSink s;
        Book<VectorSink> b(s, flow.size());
        std::size_t events = 0;
        bool found = false;
        for (std::size_t i = 0; i < flow.size() && !found; ++i) {
            apply_to(b, flow[i]);
            for (std::size_t k = 0; k < s.events.size(); ++k)
                if (i >= 1000 && s.events[k].type == EventType::Trade) {
                    want_cmd = i;
                    want_event = events + k;
                    want_ref = s.events[k];
                    found = true;
                    break;
                }
            events += s.events.size();
            s.events.clear();
        }
        REQUIRE(found);
    }
    Events wrong_out, ref_out;
    WrongBook wrong(wrong_out, 1000, false);
    ReferenceBook ref(ref_out);
    const DiffResult r = compare_run(wrong, wrong_out, ref, ref_out, flow, 5);
    REQUIRE(r.mismatch.has_value());
    CHECK_EQ(r.mismatch->command, want_cmd);
    CHECK_EQ(r.mismatch->event, want_event);
    Event bad = want_ref;
    bad.qty += 1;
    CHECK_EQ(r.mismatch->engine, to_string(bad));
    CHECK_EQ(r.mismatch->reference, to_string(want_ref));
    const std::string report = difftest_report(r);
    CHECK(report.find("MISMATCH at command " + std::to_string(want_cmd)) != std::string::npos);
    CHECK(report.find("engine:    " + to_string(bad)) != std::string::npos);
    CHECK(report.find("reference: " + to_string(want_ref)) != std::string::npos);
}

TEST("compare_run reports a difference in the final book alone (same events)") {
    const auto flow = make_flow(6, 5'000);
    Events wrong_out, ref_out;
    WrongBook wrong(wrong_out, 0, true);
    ReferenceBook ref(ref_out);
    const DiffResult r = compare_run(wrong, wrong_out, ref, ref_out, flow, 6, 0);  // final book only
    REQUIRE(r.mismatch.has_value());
    CHECK_EQ(r.mismatch->command, flow.size() - 1);
    CHECK_EQ(r.mismatch->engine.find("(none)") != std::string::npos, true);
    CHECK_EQ(r.orders, flow.size());  // every command's events agreed before the book check
}

TEST("the seeded flow is deterministic and exercises every path") {
    const auto f1 = make_flow(9, 50'000);
    const auto f2 = make_flow(9, 50'000);
    const auto f3 = make_flow(10, 50'000);
    REQUIRE(f1.size() == 50'000);
    bool same = true, differ = false;
    for (std::size_t i = 0; i < f1.size(); ++i) {
        same = same && f1[i].kind == f2[i].kind && f1[i].order.id == f2[i].order.id &&
               f1[i].order.price == f2[i].order.price && f1[i].order.qty == f2[i].order.qty &&
               f1[i].order.type == f2[i].order.type && f1[i].order.side == f2[i].order.side &&
               f1[i].target == f2[i].target && f1[i].price == f2[i].price && f1[i].qty == f2[i].qty;
        differ = differ || f1[i].order.price != f3[i].order.price || f1[i].kind != f3[i].kind;
    }
    CHECK(same);
    CHECK(differ);

    std::array<std::size_t, 3> kinds{};
    std::array<std::size_t, 4> otypes{};
    std::array<std::size_t, 5> types{};
    std::array<std::size_t, 6> reasons{};
    std::size_t market_expired = 0, ioc_expired = 0;
    VectorSink sink;
    Book<VectorSink> book(sink, f1.size());
    for (const Command& c : f1) {
        ++kinds[static_cast<std::size_t>(c.kind)];
        if (c.kind == CommandKind::New) ++otypes[static_cast<std::size_t>(c.order.type)];
        apply_to(book, c);
        for (const Event& e : sink.events) {
            ++types[static_cast<std::size_t>(e.type)];
            if (e.type == EventType::Rejected) ++reasons[static_cast<std::size_t>(e.reason)];
            if (e.type == EventType::Expired && c.kind == CommandKind::New) {
                if (c.order.type == OrderType::Market) ++market_expired;
                if (c.order.type == OrderType::IOC) ++ioc_expired;
            }
        }
        sink.events.clear();
    }
    for (std::size_t k = 0; k < kinds.size(); ++k) CHECK(kinds[k] > 0);
    for (std::size_t t = 0; t < otypes.size(); ++t) CHECK(otypes[t] > 0);
    for (std::size_t t = 0; t < types.size(); ++t) CHECK(types[t] > 0);
    for (std::size_t r = 1; r < reasons.size(); ++r) CHECK(reasons[r] > 0);
    CHECK(market_expired > 0);
    CHECK(ioc_expired > 0);
    // The book stays a realistic size (the generator retires old orders).
    CHECK(book.resting() > 50);
    CHECK(book.resting() < 5'000);
}

TEST("differential: engine and reference agree on every event and the final book, 4 seeds") {
    std::size_t orders = 0, trades = 0, events = 0;
    for (std::uint64_t seed : kSeeds) {
        // The comparison, done here in the test, command by command.
        const auto flow = make_flow(seed, kOrdersPerSeed);
        VectorSink es;
        Book<VectorSink> engine(es, flow.size());
        Events rs;
        ReferenceBook reference(rs);
        std::size_t o = 0, t = 0, ev = 0, bad = 0;
        for (std::size_t i = 0; i < flow.size(); ++i) {
            apply_to(engine, flow[i]);
            apply_to(reference, flow[i]);
            if (const auto m = first_difference(es.events, rs, i, ev)) {
                if (bad++ == 0)
                    std::fprintf(stderr, "    seed %llu command %zu: engine %s / reference %s\n",
                                 static_cast<unsigned long long>(seed), i, m->engine.c_str(), m->reference.c_str());
            }
            for (const Event& e : es.events) t += e.type == EventType::Trade ? 1 : 0;
            ev += es.events.size();
            ++o;
            es.events.clear();
            rs.clear();
        }
        CHECK_EQ(bad, std::size_t{0});
        CHECK(!snapshot_difference(engine.snapshot(), reference.snapshot(), flow.size()).has_value());
        CHECK_EQ(engine.resting(), reference.resting());
        CHECK_EQ(o, kOrdersPerSeed);
        CHECK(t > kOrdersPerSeed / 10);

        if (seed == kSeeds[0]) {
            // run_difftest (what `me difftest` uses) must count exactly the same.
            const DiffResult r = run_difftest(seed, kOrdersPerSeed);
            CHECK(!r.mismatch.has_value());
            CHECK_EQ(r.orders, o);
            CHECK_EQ(r.trades, t);
            CHECK_EQ(r.events, ev);
            CHECK_EQ(r.resting, engine.resting());
            CHECK_EQ(difftest_report(r),
                     "difftest: seed " + std::to_string(seed) + ": " + with_commas(o) + " orders, " + with_commas(t) +
                         " trades, " + with_commas(ev) + " events, " + with_commas(engine.resting()) +
                         " resting at the end: engine and reference agree on every event and on the final book\n");
        }
        orders += o;
        trades += t;
        events += ev;
    }
#ifndef ME_SANITIZE
    CHECK(orders >= 2'000'000);
#endif
    std::printf("    checked %s orders, %s trades, %s events: all agree\n", with_commas(orders).c_str(),
                with_commas(trades).c_str(), with_commas(events).c_str());
}

TEST("with_commas formats counts for the summary line") {
    CHECK_EQ(with_commas(0), std::string("0"));
    CHECK_EQ(with_commas(999), std::string("999"));
    CHECK_EQ(with_commas(1000), std::string("1,000"));
    CHECK_EQ(with_commas(2000000), std::string("2,000,000"));
    CHECK_EQ(with_commas(1234567890), std::string("1,234,567,890"));
}
