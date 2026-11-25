// me: the matching-engine command-line tool. See the README and `me help`.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "me/bench.hpp"
#include "me/difftest.hpp"
#include "me/replay.hpp"
#include "me/text.hpp"

namespace {

constexpr int kUsageError = 2;

struct CliError {
    std::string message;
};

[[noreturn]] void fail(const std::string& message) { throw CliError{message}; }

// --name value options; every option must be known and have a value.
std::map<std::string, std::string> options(const std::vector<std::string>& args, std::size_t from,
                                           const std::vector<std::string>& known,
                                           std::vector<std::string>* positional = nullptr) {
    std::map<std::string, std::string> out;
    for (std::size_t i = from; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.rfind("--", 0) != 0) {
            if (positional == nullptr) fail("unexpected argument '" + a + "'");
            positional->push_back(a);
            continue;
        }
        const std::string name = a.substr(2);
        bool ok = false;
        for (const auto& k : known) ok = ok || k == name;
        if (!ok) fail("unknown option '" + a + "'");
        if (i + 1 >= args.size()) fail("option '" + a + "' needs a value");
        out[name] = args[++i];
    }
    return out;
}

std::uint64_t number(const std::map<std::string, std::string>& o, const std::string& name, std::uint64_t def,
                     std::uint64_t min = 0) {
    const auto it = o.find(name);
    if (it == o.end()) return def;
    const auto v = me::parse_uint(it->second);
    if (!v || *v < min) fail("bad value '" + it->second + "' for --" + name);
    return *v;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) fail("cannot read '" + path + "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---- me ladder <file> ----
int cmd_ladder(const std::vector<std::string>& args) {
    std::vector<std::string> files;
    options(args, 2, {}, &files);
    if (files.size() != 1) fail("usage: me ladder <scenario file>");
    const std::string text = read_file(files[0]);

    // Parse and check the whole file first, so a bad line stops the run before any output.
    struct Line {
        std::size_t number;
        me::ParsedLine parsed;
    };
    std::vector<Line> lines;
    std::set<me::OrderId> submitted;
    std::istringstream in(text);
    std::string raw;
    for (std::size_t n = 1; std::getline(in, raw); ++n) {
        me::ParsedLine p = me::parse_line(raw);
        if (p.empty) continue;
        if (!p.error.empty()) fail("line " + std::to_string(n) + ": " + p.error);
        if (p.command.kind == me::CommandKind::New) submitted.insert(p.command.order.id);
        else if (submitted.count(p.command.target) == 0)
            fail("line " + std::to_string(n) + ": unknown order id " + std::to_string(p.command.target));
        lines.push_back({n, std::move(p)});
    }
    if (lines.empty()) fail("'" + files[0] + "' has no commands");

    me::VectorSink sink;
    me::Book<me::VectorSink> book(sink);
    std::size_t i = 0;
    for (const Line& l : lines) {
        std::printf("[%zu] %s\n", ++i, l.parsed.text.c_str());
        me::apply_one(book, l.parsed.command);
        if (sink.events.empty()) std::printf("    (no events)\n");
        for (const me::Event& e : sink.events) std::printf("    %s\n", me::to_string(e).c_str());
        sink.events.clear();
        std::ostringstream os;
        me::print_ladder(book.snapshot(), os, "      ");
        std::fputs(os.str().c_str(), stdout);
        std::printf("\n");
    }
    return 0;
}

// ---- me difftest ----
int cmd_difftest(const std::vector<std::string>& args) {
    const auto o = options(args, 2, {"orders", "seed"});
    const std::uint64_t orders = number(o, "orders", 2'000'000, 1);
    const std::uint64_t seed = number(o, "seed", 1);
    const me::DiffResult r = me::run_difftest(seed, orders);
    std::fputs(me::difftest_report(r).c_str(), stdout);
    return r.mismatch ? 1 : 0;
}

// ---- me bench ----
int cmd_bench(const std::vector<std::string>& args) {
    const auto o = options(args, 2, {"orders", "seed", "warmup", "runs", "json"});
    const std::uint64_t n = number(o, "orders", 1'000'000, 1);
    const std::uint64_t seed = number(o, "seed", 42);
    const std::uint64_t warmup = number(o, "warmup", 1);
    const std::uint64_t runs = number(o, "runs", 5, 1);
    const std::string json_path = o.count("json") ? o.at("json") : "";

    const me::Machine m = me::machine();
    const me::TimerInfo timer = me::measure_timer();
    const me::FlowMix mix;
    const auto flow = me::make_flow(seed, n, mix);

    for (std::uint64_t i = 0; i < warmup; ++i) (void)me::throughput_run(flow);
    std::vector<double> rates;
    me::RunResult first{};
    for (std::uint64_t i = 0; i < runs; ++i) {
        const me::RunResult r = me::throughput_run(flow);
        if (i == 0) first = r;
        if (r.checksum != first.checksum) {
            std::fprintf(stderr, "error: run %llu gave checksum %s, run 1 gave %s: the engine is not deterministic\n",
                         static_cast<unsigned long long>(i + 1), me::hex64(r.checksum).c_str(),
                         me::hex64(first.checksum).c_str());
            return 1;
        }
        rates.push_back(static_cast<double>(n) / r.seconds);
    }
    const me::LatencyResult lat = me::latency_pass(flow, timer.resolution_ns);
    const double med = me::median(rates);
    const double lo = *std::min_element(rates.begin(), rates.end());
    const double hi = *std::max_element(rates.begin(), rates.end());

    std::printf("matching-engine benchmark\n");
    std::printf("  machine   %s\n", m.describe().c_str());
    std::printf("  compiler  %s\n", ME_COMPILER);
    std::printf("  flags     %s (%s build)\n", ME_CXX_FLAGS, ME_BUILD_TYPE);
    std::printf("  timer     std::chrono::steady_clock, resolution %s ns, overhead %s ns per call\n",
                me::fixed(timer.resolution_ns, 1).c_str(), me::fixed(timer.overhead_ns, 1).c_str());
    std::printf("  flow      seed %llu, %s orders: %s\n", static_cast<unsigned long long>(seed),
                me::with_commas(n).c_str(), mix.describe().c_str());
    std::printf("  checksum  %s (%s events, %s trades)\n", me::hex64(first.checksum).c_str(),
                me::with_commas(first.events).c_str(), me::with_commas(first.trades).c_str());
    std::printf("  runs      %llu warm-up + %llu timed (fresh book each run; latency from one extra timed pass)\n\n",
                static_cast<unsigned long long>(warmup), static_cast<unsigned long long>(runs));
    std::printf("  | metric                     | value                          |\n");
    std::printf("  |----------------------------|--------------------------------|\n");
    std::printf("  | orders/sec (median)        | %-30s |\n", me::millions(med).c_str());
    std::printf("  | orders/sec (min .. max)    | %-30s |\n", (me::millions(lo) + " .. " + me::millions(hi)).c_str());
    // A reading below half a timer step is a reading of 0 steps: say so instead of "0 ns".
    auto cell = [&](double v) {
        if (v < 0.5 * timer.resolution_ns) return "< " + me::fixed(timer.resolution_ns, 0) + " ns (below 1 timer step)";
        return me::fixed(v, 0) + " ns";
    };
    std::printf("  | latency p50                | %-30s |\n", cell(lat.p50).c_str());
    std::printf("  | latency p90                | %-30s |\n", cell(lat.p90).c_str());
    std::printf("  | latency p99                | %-30s |\n", cell(lat.p99).c_str());
    std::printf("  | latency p99.9              | %-30s |\n", cell(lat.p999).c_str());
    std::printf("  | latency max                | %-30s |\n", cell(lat.max).c_str());
    std::printf("  | mean ns/order (throughput) | %-30s |\n\n", (me::fixed(1e9 / med, 1) + " ns").c_str());
    std::printf("  per-order latency: one timestamp pair per order, so each reading is a whole number of\n"
                "  timer steps (%s ns) and includes about %s ns of timer overhead\n",
                me::fixed(timer.resolution_ns, 1).c_str(), me::fixed(timer.overhead_ns, 1).c_str());
    std::uint64_t biggest = 1;
    for (const auto& b : lat.buckets) biggest = std::max(biggest, b.count);
    for (const auto& b : lat.buckets) {
        const int bar = static_cast<int>(40.0 * static_cast<double>(b.count) / static_cast<double>(biggest) + 0.5);
        std::printf("  %-16s %-40s %s\n", me::bucket_label(b).c_str(), std::string(static_cast<std::size_t>(bar), '#').c_str(),
                    me::with_commas(b.count).c_str());
    }

    if (!json_path.empty()) {
        std::ofstream js(json_path);
        if (!js) fail("cannot write '" + json_path + "'");
        js << "{\n";
        js << "  \"machine\": \"" << me::json_escape(m.describe()) << "\",\n";
        js << "  \"compiler\": \"" << me::json_escape(ME_COMPILER) << "\",\n";
        js << "  \"flags\": \"" << me::json_escape(ME_CXX_FLAGS) << "\",\n";
        js << "  \"build_type\": \"" << ME_BUILD_TYPE << "\",\n";
        js << "  \"seed\": " << seed << ",\n  \"orders\": " << n << ",\n";
        js << "  \"warmup\": " << warmup << ",\n  \"runs\": " << runs << ",\n";
        js << "  \"flow_mix\": \"" << mix.describe() << "\",\n";
        js << "  \"checksum\": \"" << me::hex64(first.checksum) << "\",\n";
        js << "  \"events\": " << first.events << ",\n  \"trades\": " << first.trades << ",\n";
        js << "  \"timer\": {\"clock\": \"std::chrono::steady_clock\", \"resolution_ns\": " << me::fixed(timer.resolution_ns, 2)
           << ", \"overhead_ns\": " << me::fixed(timer.overhead_ns, 2) << "},\n";
        js << "  \"orders_per_sec\": {\"median\": " << me::fixed(med, 0) << ", \"min\": " << me::fixed(lo, 0)
           << ", \"max\": " << me::fixed(hi, 0) << ", \"all\": [";
        for (std::size_t i = 0; i < rates.size(); ++i) js << (i ? ", " : "") << me::fixed(rates[i], 0);
        js << "]},\n";
        js << "  \"latency_ns\": {\"p50\": " << me::fixed(lat.p50, 1) << ", \"p90\": " << me::fixed(lat.p90, 1)
           << ", \"p99\": " << me::fixed(lat.p99, 1) << ", \"p99.9\": " << me::fixed(lat.p999, 1)
           << ", \"max\": " << me::fixed(lat.max, 1) << ", \"mean\": " << me::fixed(lat.mean, 2) << "},\n";
        js << "  \"ns_per_order_throughput\": " << me::fixed(1e9 / med, 2) << ",\n";
        js << "  \"histogram\": [";
        for (std::size_t i = 0; i < lat.buckets.size(); ++i) {
            const auto& b = lat.buckets[i];
            js << (i ? ",\n    " : "\n    ") << "{\"lo_ns\": " << me::fixed(b.lo_ns, 0) << ", \"hi_ns\": "
               << (b.hi_ns < 0 ? std::string("null") : me::fixed(b.hi_ns, 0)) << ", \"label\": \""
               << me::bucket_label(b) << "\", \"count\": " << b.count << "}";
        }
        js << "\n  ]\n}\n";
    }
    return 0;
}

// ---- me replay ----
int cmd_replay(const std::vector<std::string>& args) {
    std::vector<std::string> files;
    const auto o = options(args, 2, {"runs"}, &files);
    const std::uint64_t runs = number(o, "runs", 20, 1);
    if (files.empty()) fail("usage: me replay <fills.csv>... [--runs R]");
    std::vector<me::ReplayFile> parsed;
    for (const std::string& f : files) {
        std::string err;
        auto rf = me::parse_fills_csv(f, read_file(f), err);
        if (!rf) fail(err);
        parsed.push_back(std::move(*rf));
    }
    const me::ReplayResult r = me::run_replay(parsed, runs);
    if (!r.problem.empty()) {
        std::printf("replay: MISMATCH: %s\n", r.problem.c_str());
        return 1;
    }
    std::printf("replay: %zu file(s), %s fills, %s orders, %s trades, inventory matches\n", parsed.size(),
                me::with_commas(r.fills).c_str(), me::with_commas(r.orders).c_str(), me::with_commas(r.trades).c_str());
    std::printf("replay: %s orders/sec (median of %llu timed replays; min %s, max %s; Release build)\n",
                me::millions(r.median_rate).c_str(), static_cast<unsigned long long>(runs),
                me::millions(r.min_rate).c_str(), me::millions(r.max_rate).c_str());
    return 0;
}

void usage() {
    std::puts(
        "usage: me <command> [options]\n"
        "  ladder <file>                     run a scenario; print the book as a price ladder after each order\n"
        "  difftest [--orders N] [--seed S]  run N seeded orders through the engine and the reference matcher\n"
        "  bench [--orders N] [--seed S] [--warmup W] [--runs R] [--json PATH]\n"
        "                                    throughput and per-order latency (use ./bench for the full report)\n"
        "  replay <fills.csv>... [--runs R]  replay market-maker-sim fills through the engine");
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv, argv + argc);
    try {
        if (args.size() < 2) {
            usage();
            return kUsageError;
        }
        const std::string& cmd = args[1];
        if (cmd == "ladder") return cmd_ladder(args);
        if (cmd == "difftest") return cmd_difftest(args);
        if (cmd == "bench") return cmd_bench(args);
        if (cmd == "replay") return cmd_replay(args);
        if (cmd == "help" || cmd == "--help" || cmd == "-h") {
            usage();
            return 0;
        }
        fail("unknown command '" + cmd + "' (try: me help)");
    } catch (const CliError& e) {
        std::fprintf(stderr, "error: %s\n", e.message.c_str());
        return kUsageError;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kUsageError;
    }
}
