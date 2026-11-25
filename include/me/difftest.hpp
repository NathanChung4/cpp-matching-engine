// The differential test: the same seeded commands go through the fast book and the
// reference matcher, and every event and the final book must be identical.
// Events are compared after every command (and then dropped), so millions of orders run
// in constant memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "me/book.hpp"
#include "me/events.hpp"
#include "me/flow.hpp"
#include "me/reference.hpp"
#include "me/text.hpp"

namespace me {

struct Mismatch {
    std::size_t command = 0;  // index of the command where the streams first differ
    std::size_t event = 0;    // index of the first differing event (over the whole run)
    std::string engine;       // that event as the engine printed it ("(none)" if missing)
    std::string reference;    // and as the reference printed it
};

struct DiffResult {
    std::uint64_t seed = 0;
    std::size_t orders = 0;  // commands run (new orders, cancels, modifies)
    std::size_t trades = 0;
    std::size_t events = 0;
    std::size_t resting = 0;
    std::optional<Mismatch> mismatch;
};

// First difference between two event lists, or nullopt. `offset` is the global index of
// the lists' first event.
inline std::optional<Mismatch> first_difference(const std::vector<Event>& engine, const std::vector<Event>& ref,
                                                std::size_t command, std::size_t offset) {
    const std::size_t n = std::max(engine.size(), ref.size());
    for (std::size_t i = 0; i < n; ++i) {
        const bool have_e = i < engine.size(), have_r = i < ref.size();
        if (have_e && have_r && engine[i] == ref[i]) continue;
        return Mismatch{command, offset + i, have_e ? to_string(engine[i]) : "(none)",
                        have_r ? to_string(ref[i]) : "(none)"};
    }
    return std::nullopt;
}

inline std::optional<Mismatch> snapshot_difference(const Snapshot& engine, const Snapshot& ref, std::size_t command) {
    const std::size_t n = std::max(engine.size(), ref.size());
    for (std::size_t i = 0; i < n; ++i) {
        const bool have_e = i < engine.size(), have_r = i < ref.size();
        if (have_e && have_r && engine[i] == ref[i]) continue;
        return Mismatch{command, i, "book[" + std::to_string(i) + "] " + (have_e ? to_string(engine[i]) : "(none)"),
                        "book[" + std::to_string(i) + "] " + (have_r ? to_string(ref[i]) : "(none)")};
    }
    return std::nullopt;
}

template <class Engine, class Reference>
void apply(Engine& e, Reference& r, const Command& c) {
    switch (c.kind) {
        case CommandKind::New: e.submit(c.order); r.submit(c.order); break;
        case CommandKind::Cancel: e.cancel(c.target); r.cancel(c.target); break;
        case CommandKind::Modify: e.modify(c.target, c.price, c.qty); r.modify(c.target, c.price, c.qty); break;
    }
}

// Runs `commands` through any two matchers that write their events into `engine_out` and
// `ref_out` (the seam the tests use to inject a wrong matcher). Compares the events after
// every command, and the whole book every `snapshot_every` commands and at the end
// (`snapshot_every` = 0: only the final book is compared).
template <class Engine, class Reference>
DiffResult compare_run(Engine& engine, std::vector<Event>& engine_out, Reference& reference,
                       std::vector<Event>& ref_out, const std::vector<Command>& commands, std::uint64_t seed = 0,
                       std::size_t snapshot_every = 100'000) {
    DiffResult res;
    res.seed = seed;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        apply(engine, reference, commands[i]);
        if (auto m = first_difference(engine_out, ref_out, i, res.events)) {
            res.mismatch = m;
            return res;
        }
        for (const Event& ev : engine_out) res.trades += ev.type == EventType::Trade ? 1 : 0;
        res.events += engine_out.size();
        engine_out.clear();
        ref_out.clear();
        ++res.orders;
        const bool last = i + 1 == commands.size();
        if (last || (snapshot_every != 0 && (i + 1) % snapshot_every == 0)) {
            if (auto m = snapshot_difference(engine.snapshot(), reference.snapshot(), i)) {
                res.mismatch = m;
                return res;
            }
        }
    }
    res.resting = engine.resting();
    return res;
}

// The fast book against the reference matcher.
inline DiffResult run_difftest(const std::vector<Command>& commands, std::uint64_t seed = 0,
                               std::size_t snapshot_every = 100'000) {
    VectorSink es;
    Book<VectorSink> engine(es, commands.size() + 16);
    std::vector<Event> rs;
    ReferenceBook reference(rs);
    return compare_run(engine, es.events, reference, rs, commands, seed, snapshot_every);
}

inline DiffResult run_difftest(std::uint64_t seed, std::size_t orders) {
    return run_difftest(make_flow(seed, orders), seed);
}

// What `me difftest` prints (see README).
inline std::string difftest_report(const DiffResult& r) {
    const std::string seed = std::to_string(r.seed);
    if (r.mismatch) {
        return "difftest: seed " + seed + ": MISMATCH at command " + std::to_string(r.mismatch->command) +
               ", event " + std::to_string(r.mismatch->event) + "\n  engine:    " + r.mismatch->engine +
               "\n  reference: " + r.mismatch->reference + "\n";
    }
    return "difftest: seed " + seed + ": " + with_commas(r.orders) + " orders, " + with_commas(r.trades) +
           " trades, " + with_commas(r.events) + " events, " + with_commas(r.resting) +
           " resting at the end: engine and reference agree on every event and on the final book\n";
}

}  // namespace me
