// Replays market-maker-sim's fills (results/fills_*.csv) through the engine (DECISIONS.md
// D6). In those files `price` is integer ticks of $0.01 and `side` is the
// market maker's side. Each fill becomes two orders: the market maker's 1-lot quote, which
// rests, and the trader's 1-lot IOC on the other side at the same price, which takes it.
// The replay checks every fill became exactly that trade and that the market maker's
// inventory after each trade equals the CSV's `inventory` column.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "me/book.hpp"
#include "me/events.hpp"
#include "me/text.hpp"

namespace me {

struct Fill {
    Side mm_side;
    Price price;
    std::int64_t inventory;  // the market maker's inventory after this fill (from the CSV)
    std::size_t row;         // 1-based data row, for error messages
};

struct ReplayFile {
    std::string path;
    std::vector<Fill> fills;
};

inline std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

inline std::optional<std::int64_t> parse_int(const std::string& s) {
    if (!s.empty() && s[0] == '-') {
        const auto v = parse_uint(std::string_view(s).substr(1));
        if (!v || *v > static_cast<std::uint64_t>(INT64_MAX)) return std::nullopt;
        return -static_cast<std::int64_t>(*v);
    }
    const auto v = parse_uint(s);
    if (!v || *v > static_cast<std::uint64_t>(INT64_MAX)) return std::nullopt;
    return static_cast<std::int64_t>(*v);
}

// Parses one fills CSV. On a problem returns nullopt and sets `err` to one line.
inline std::optional<ReplayFile> parse_fills_csv(const std::string& path, const std::string& text, std::string& err) {
    ReplayFile rf{path, {}};
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line)) {
        err = path + ": empty file";
        return std::nullopt;
    }
    const auto header = split_csv(line);
    auto col = [&](const char* name) -> int {
        for (std::size_t i = 0; i < header.size(); ++i)
            if (header[i] == name) return static_cast<int>(i);
        return -1;
    };
    const int c_side = col("side"), c_price = col("price"), c_inv = col("inventory");
    if (c_side < 0 || c_price < 0 || c_inv < 0) {
        err = path + ": not a market-maker-sim fills file (needs side, price and inventory columns)";
        return std::nullopt;
    }
    std::size_t row = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        ++row;
        const auto f = split_csv(line);
        const std::size_t need = static_cast<std::size_t>(std::max({c_side, c_price, c_inv})) + 1;
        if (f.size() < need) {
            err = path + ": row " + std::to_string(row) + ": too few columns";
            return std::nullopt;
        }
        const std::string& s = f[static_cast<std::size_t>(c_side)];
        if (s != "buy" && s != "sell") {
            err = path + ": row " + std::to_string(row) + ": bad side '" + s + "'";
            return std::nullopt;
        }
        const auto p = parse_int(f[static_cast<std::size_t>(c_price)]);
        if (!p || *p < kMinPrice || *p > kMaxPrice) {
            err = path + ": row " + std::to_string(row) + ": bad price '" + f[static_cast<std::size_t>(c_price)] + "'";
            return std::nullopt;
        }
        const auto inv = parse_int(f[static_cast<std::size_t>(c_inv)]);
        if (!inv) {
            err = path + ": row " + std::to_string(row) + ": bad inventory '" + f[static_cast<std::size_t>(c_inv)] + "'";
            return std::nullopt;
        }
        rf.fills.push_back({s == "buy" ? Side::Buy : Side::Sell, *p, *inv, row});
    }
    if (rf.fills.empty()) {
        err = path + ": no fills";
        return std::nullopt;
    }
    return rf;
}

struct ReplayResult {
    std::size_t fills = 0, orders = 0, trades = 0;
    double median_rate = 0, min_rate = 0, max_rate = 0;
    std::string problem;  // non-empty: the first fill that did not replay as the CSV says
};

// The engine orders for all files: ids 1, 2, 3, ... (maker, then taker, per fill).
inline std::vector<NewOrder> replay_orders(const std::vector<ReplayFile>& files) {
    std::vector<NewOrder> out;
    OrderId id = 1;
    for (const ReplayFile& f : files)
        for (const Fill& x : f.fills) {
            out.push_back({id++, x.mm_side, OrderType::Limit, x.price, 1});
            out.push_back({id++, opposite(x.mm_side), OrderType::IOC, x.price, 1});
        }
    return out;
}

inline ReplayResult run_replay(const std::vector<ReplayFile>& files, std::uint64_t runs) {
    ReplayResult r;
    const std::vector<NewOrder> orders = replay_orders(files);
    r.orders = orders.size();

    // 1. Check: every fill is one trade at the CSV price, and the inventory follows the CSV.
    VectorSink sink;
    Book<VectorSink> book(sink, orders.size() + 16);
    std::size_t k = 0;
    for (const ReplayFile& f : files) {
        std::int64_t inventory = 0;  // each file is its own simulation, starting flat
        for (const Fill& x : f.fills) {
            const NewOrder& maker = orders[k++];
            const NewOrder& taker = orders[k++];
            book.submit(maker);
            book.submit(taker);
            const std::vector<Event> want{Event::rest(maker.id, maker.side, x.price, 1),
                                          Event::trade(taker.id, maker.id, taker.side, x.price, 1)};
            if (sink.events != want) {
                r.problem = f.path + ": row " + std::to_string(x.row) + ": expected one trade at " +
                            price_text(x.price) + ", got " + std::to_string(sink.events.size()) + " event(s)";
                return r;
            }
            ++r.trades;
            ++r.fills;
            inventory += x.mm_side == Side::Buy ? 1 : -1;
            if (inventory != x.inventory) {
                r.problem = f.path + ": row " + std::to_string(x.row) + ": inventory " + std::to_string(inventory) +
                            " after the trade, the CSV says " + std::to_string(x.inventory);
                return r;
            }
            sink.events.clear();
        }
    }

    // 2. Time it: fresh book per run, no per-order timer.
    std::vector<double> rates;
    for (std::uint64_t i = 0; i < runs + 1; ++i) {  // the first run is a warm-up
        ChecksumSink cs;
        Book<ChecksumSink> b(cs, orders.size() + 16);
        const auto t0 = std::chrono::steady_clock::now();
        for (const NewOrder& o : orders) b.submit(o);
        const auto t1 = std::chrono::steady_clock::now();
        if (i == 0) continue;
        rates.push_back(static_cast<double>(orders.size()) / std::chrono::duration<double>(t1 - t0).count());
    }
    std::sort(rates.begin(), rates.end());
    r.min_rate = rates.front();
    r.max_rate = rates.back();
    const std::size_t n = rates.size();
    r.median_rate = n % 2 ? rates[n / 2] : (rates[n / 2 - 1] + rates[n / 2]) / 2;
    return r;
}

}  // namespace me
