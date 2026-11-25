// Seeded synthetic order flow, used by the differential test and the benchmark.
//
// The random generator and every draw are written out here (splitmix64 plus integer
// arithmetic, no std::*_distribution), because the standard distributions are allowed to
// differ between library versions: the same seed must give the same orders everywhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "me/types.hpp"

namespace me {

enum class CommandKind : std::uint8_t { New, Cancel, Modify };

struct Command {
    CommandKind kind{};
    NewOrder order{};    // New
    OrderId target{};    // Cancel, Modify
    Price price{};       // Modify
    Qty qty{};           // Modify
};

class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (s_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    // Uniform in [0, n). The modulo bias is below 2^-40 for the small n used here.
    std::uint64_t below(std::uint64_t n) { return next() % n; }

private:
    std::uint64_t s_;
};

// The mix, in parts per 1000 (must sum to 1000).
struct FlowMix {
    int limit = 470;
    int market = 60;
    int ioc = 60;
    int fok = 40;
    int cancel = 288;
    int modify = 80;
    int bad = 2;  // malformed or invalid commands, so the reject paths are compared too
    std::string describe() const {
        auto pct = [](int x) { return std::to_string(x / 10) + "." + std::to_string(x % 10) + "%"; };
        return pct(limit) + " limit, " + pct(market) + " market, " + pct(ioc) + " IOC, " + pct(fok) +
               " FOK, " + pct(cancel) + " cancel, " + pct(modify) + " modify, " + pct(bad) + " invalid";
    }
};

// Generates `n` commands around a mid price that random-walks from $100.00.
// Every limit order lives for at most `max_live` later limit orders: the generator then
// cancels it if it is still there (a cancel of a filled order is a reject, which is fine),
// so the book stays a realistic size instead of growing forever.
inline std::vector<Command> make_flow(std::uint64_t seed, std::size_t n, FlowMix mix = {},
                                      std::size_t max_live = 1000) {
    Rng rng(seed);
    std::vector<Command> out;
    out.reserve(n);
    std::deque<OrderId> live;  // limit-order ids in arrival order (some already filled)
    Price mid = 10'000;
    OrderId next_id = 1;
    const int b1 = mix.limit, b2 = b1 + mix.market, b3 = b2 + mix.ioc, b4 = b3 + mix.fok,
              b5 = b4 + mix.cancel, b6 = b5 + mix.modify;

    auto qty = [&]() -> Qty {
        return static_cast<Qty>(rng.below(20) == 0 ? 1 + rng.below(1000) : 1 + rng.below(100));
    };
    // Offset from mid, away from the spread: mostly 0..20 ticks, sometimes a few ticks
    // through the mid, so some limits cross and trade.
    auto price_for = [&](Side s) -> Price {
        const Price off = static_cast<Price>(rng.below(24)) - 3;
        return s == Side::Buy ? mid - off : mid + off;
    };
    auto pick_live = [&]() -> OrderId {
        return live.empty() ? next_id + 1'000'000 : live[rng.below(live.size())];
    };

    while (out.size() < n) {
        if (rng.below(10) == 0) mid += rng.below(2) == 0 ? -1 : 1;
        if (mid < 200) mid = 200;
        if (mid > kMaxPrice - 200) mid = kMaxPrice - 200;

        if (live.size() > max_live) {
            out.push_back({CommandKind::Cancel, {}, live.front(), 0, 0});
            live.pop_front();
            continue;
        }
        const int r = static_cast<int>(rng.below(1000));
        const Side side = rng.below(2) == 0 ? Side::Buy : Side::Sell;
        Command c{};
        if (r < b1 || r >= b6) {
            if (r >= b6) {
                // Invalid on purpose: zero qty, out-of-band price, a reused id, or a cancel
                // of an id never sent.
                switch (rng.below(4)) {
                    case 0: c = {CommandKind::New, {next_id++, side, OrderType::Limit, mid, 0}, 0, 0, 0}; break;
                    case 1: c = {CommandKind::New, {next_id++, side, OrderType::Limit, kMaxPrice + 5, 10}, 0, 0, 0}; break;
                    case 2: c = {CommandKind::New, {next_id > 1 ? next_id - 1 : 1, side, OrderType::Limit, mid, 10}, 0, 0, 0}; break;
                    default: c = {CommandKind::Cancel, {}, next_id + 5'000'000, 0, 0}; break;
                }
                out.push_back(c);
                continue;
            }
            const OrderId id = next_id++;
            c = {CommandKind::New, {id, side, OrderType::Limit, price_for(side), qty()}, 0, 0, 0};
            live.push_back(id);
        } else if (r < b2) {
            c = {CommandKind::New, {next_id++, side, OrderType::Market, 0, qty()}, 0, 0, 0};
        } else if (r < b3) {
            c = {CommandKind::New, {next_id++, side, OrderType::IOC, price_for(side), qty()}, 0, 0, 0};
        } else if (r < b4) {
            c = {CommandKind::New, {next_id++, side, OrderType::FOK, price_for(side), qty()}, 0, 0, 0};
        } else if (r < b5) {
            c = {CommandKind::Cancel, {}, pick_live(), 0, 0};
        } else {
            // Modify needs the order's side for a sensible price; the generator does not
            // track it, so it picks a price near mid on a random side: sometimes the new
            // price crosses and trades, which is what we want to exercise.
            c = {CommandKind::Modify, {}, pick_live(), price_for(side), qty()};
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace me
