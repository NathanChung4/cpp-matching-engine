// Text in and out for the `me` tool: strict number parsing, the scenario line format, and
// the price ladder.
#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "me/events.hpp"
#include "me/flow.hpp"
#include "me/types.hpp"

namespace me {

// Sends one command to any book (the fast one or the reference).
template <class B>
inline void apply_one(B& book, const Command& c) {
    switch (c.kind) {
        case CommandKind::New: book.submit(c.order); break;
        case CommandKind::Cancel: book.cancel(c.target); break;
        case CommandKind::Modify: book.modify(c.target, c.price, c.qty); break;
    }
}

// "1234567" -> "1,234,567"
inline std::string with_commas(std::uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

// Digits only, no sign, no spaces; must fit in uint64.
inline std::optional<std::uint64_t> parse_uint(std::string_view s) {
    if (s.empty() || s.size() > 20) return std::nullopt;
    std::uint64_t v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return v;
}

// "100", "100.5", "100.05" -> ticks. At most 2 decimals, no sign.
inline std::optional<Price> parse_price(std::string_view s) {
    const std::size_t dot = s.find('.');
    const std::string_view whole = s.substr(0, dot);
    std::string_view frac = dot == std::string_view::npos ? std::string_view{} : s.substr(dot + 1);
    if (whole.empty() || whole.size() > 12) return std::nullopt;
    if (dot != std::string_view::npos && (frac.empty() || frac.size() > 2)) return std::nullopt;
    const auto w = parse_uint(whole);
    if (!w) return std::nullopt;
    std::uint64_t cents = 0;
    if (!frac.empty()) {
        const auto f = parse_uint(frac);
        if (!f) return std::nullopt;
        cents = *f * (frac.size() == 1 ? 10 : 1);
    }
    return static_cast<Price>(*w * 100 + cents);
}

inline std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

struct ParsedLine {
    bool empty = true;      // blank or comment only
    std::string error;      // non-empty: why the line is bad
    Command command{};
    std::string text;       // the command as written, comments and extra spaces removed
};

// One scenario line: see scenarios/walkthrough.txt and the README.
inline ParsedLine parse_line(std::string_view raw) {
    ParsedLine out;
    const std::string_view line = raw.substr(0, raw.find('#'));
    const auto w = words(line);
    if (w.empty()) return out;
    out.empty = false;
    for (std::size_t i = 0; i < w.size(); ++i) out.text += (i ? " " : "") + std::string(w[i]);

    auto bad = [&](const std::string& why) {
        out.error = why;
        return out;
    };
    auto side_of = [](std::string_view s) -> std::optional<Side> {
        if (s == "buy") return Side::Buy;
        if (s == "sell") return Side::Sell;
        return std::nullopt;
    };
    auto qty_of = [&](std::string_view s) -> std::optional<Qty> {
        const auto v = parse_uint(s);
        if (!v || *v > 0xFFFFFFFFULL) return std::nullopt;
        return static_cast<Qty>(*v);
    };
    const std::string_view cmd = w[0];
    auto need = [&](std::size_t n, const char* usage) -> std::string {
        if (w.size() != n) return std::string("'") + std::string(cmd) + "' needs: " + usage;
        return {};
    };

    if (cmd == "limit" || cmd == "ioc" || cmd == "fok") {
        if (auto e = need(5, "<buy|sell> <id> <price> <qty>"); !e.empty()) return bad(e);
        const auto s = side_of(w[1]);
        if (!s) return bad("bad side '" + std::string(w[1]) + "' (use buy or sell)");
        const auto id = parse_uint(w[2]);
        if (!id) return bad("bad order id '" + std::string(w[2]) + "'");
        const auto p = parse_price(w[3]);
        if (!p) return bad("bad price '" + std::string(w[3]) + "' (use a number with at most 2 decimals)");
        const auto q = qty_of(w[4]);
        if (!q) return bad("bad quantity '" + std::string(w[4]) + "'");
        const OrderType t = cmd == "limit" ? OrderType::Limit : cmd == "ioc" ? OrderType::IOC : OrderType::FOK;
        out.command = {CommandKind::New, {*id, *s, t, *p, *q}, 0, 0, 0};
    } else if (cmd == "market") {
        if (auto e = need(4, "<buy|sell> <id> <qty>"); !e.empty()) return bad(e);
        const auto s = side_of(w[1]);
        if (!s) return bad("bad side '" + std::string(w[1]) + "' (use buy or sell)");
        const auto id = parse_uint(w[2]);
        if (!id) return bad("bad order id '" + std::string(w[2]) + "'");
        const auto q = qty_of(w[3]);
        if (!q) return bad("bad quantity '" + std::string(w[3]) + "'");
        out.command = {CommandKind::New, {*id, *s, OrderType::Market, 0, *q}, 0, 0, 0};
    } else if (cmd == "cancel") {
        if (auto e = need(2, "<id>"); !e.empty()) return bad(e);
        const auto id = parse_uint(w[1]);
        if (!id) return bad("bad order id '" + std::string(w[1]) + "'");
        out.command = {CommandKind::Cancel, {}, *id, 0, 0};
    } else if (cmd == "modify") {
        if (auto e = need(4, "<id> <price> <qty>"); !e.empty()) return bad(e);
        const auto id = parse_uint(w[1]);
        if (!id) return bad("bad order id '" + std::string(w[1]) + "'");
        const auto p = parse_price(w[2]);
        if (!p) return bad("bad price '" + std::string(w[2]) + "' (use a number with at most 2 decimals)");
        const auto q = qty_of(w[3]);
        if (!q) return bad("bad quantity '" + std::string(w[3]) + "'");
        out.command = {CommandKind::Modify, {}, *id, *p, *q};
    } else {
        return bad("unknown command '" + std::string(cmd) + "' (use limit, market, ioc, fok, cancel or modify)");
    }
    return out;
}

// The book as a price ladder: asks above (highest first), the spread, bids below.
inline void print_ladder(const Snapshot& snap, std::ostream& os, const std::string& indent = "    ") {
    struct Level {
        Price price;
        std::uint64_t qty;
        std::string orders;
    };
    std::vector<Level> bids, asks;
    for (const Resting& r : snap) {
        auto& side = r.side == Side::Buy ? bids : asks;
        if (side.empty() || side.back().price != r.price) side.push_back({r.price, 0, ""});
        side.back().qty += r.qty;
        side.back().orders += (side.back().orders.empty() ? "#" : " #") + std::to_string(r.id) + ":" +
                              std::to_string(r.qty);
    }
    auto row = [&](const char* tag, const Level& l) {
        std::string p = price_text(l.price);
        std::string q = with_commas(l.qty);
        os << indent << tag << ' ' << std::string(p.size() < 8 ? 8 - p.size() : 0, ' ') << p << " | "
           << std::string(q.size() < 9 ? 9 - q.size() : 0, ' ') << q << " | " << l.orders << '\n';
    };
    if (asks.empty()) os << indent << "(no asks)\n";
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) row("ASK", *it);
    if (!asks.empty() && !bids.empty())
        os << indent << "-------------- spread " << price_text(asks.front().price - bids.front().price) << " --------------\n";
    else
        os << indent << "-------------- spread (one side empty) --------------\n";
    for (const Level& l : bids) row("BID", l);
    if (bids.empty()) os << indent << "(no bids)\n";
}

}  // namespace me
