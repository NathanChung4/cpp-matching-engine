// The benchmark: honest, reproducible numbers (DECISIONS.md D5).
//   - the seeded flow is generated before any timing starts;
//   - throughput runs have no per-order timer; each run uses a fresh book; warm-up runs
//     are done first and not counted;
//   - latency comes from one separate pass that timestamps every order;
//   - the timer's resolution and per-call overhead are measured and reported;
//   - every run must produce the same event checksum, or the benchmark fails.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <sys/sysctl.h>
#include <sys/utsname.h>

#include "me/book.hpp"
#include "me/difftest.hpp"
#include "me/events.hpp"
#include "me/flow.hpp"
#include "me/text.hpp"

namespace me {

using Clock = std::chrono::steady_clock;

inline std::string sysctl_string(const char* name) {
    char buf[256] = {};
    std::size_t len = sizeof(buf);
    if (sysctlbyname(name, buf, &len, nullptr, 0) != 0) return "unknown";
    return std::string(buf);
}
inline std::uint64_t sysctl_u64(const char* name) {
    std::uint64_t v = 0;
    std::size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return 0;
    return len == 4 ? static_cast<std::uint32_t>(v) : v;
}

struct Machine {
    std::string cpu, os, model;
    std::uint64_t cores = 0, mem_gib = 0;
    std::string describe() const {
        return cpu + ", " + std::to_string(cores) + " cores, " + std::to_string(mem_gib) + " GiB, " + os + " (" +
               model + ")";
    }
};

inline Machine machine() {
    Machine m;
    m.cpu = sysctl_string("machdep.cpu.brand_string");
    m.model = sysctl_string("hw.model");
    m.cores = sysctl_u64("hw.ncpu");
    m.mem_gib = sysctl_u64("hw.memsize") >> 30;
    m.os = "macOS " + sysctl_string("kern.osproductversion");
    return m;
}

struct TimerInfo {
    double resolution_ns = 0;  // the smallest step the clock ever shows
    double overhead_ns = 0;    // the average cost of one now() call
};

inline TimerInfo measure_timer() {
    TimerInfo t;
    double best = 1e18;
    for (int i = 0; i < 2000; ++i) {
        const auto a = Clock::now();
        auto b = Clock::now();
        while (b == a) b = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(b - a).count());
    }
    t.resolution_ns = best;
    constexpr int kCalls = 1'000'000;
    const auto s = Clock::now();
    Clock::time_point x{};
    for (int i = 0; i < kCalls; ++i) {
        x = Clock::now();
        asm volatile("" : : "r"(&x) : "memory");
    }
    t.overhead_ns = std::chrono::duration<double, std::nano>(Clock::now() - s).count() / kCalls;
    return t;
}

struct RunResult {
    double seconds = 0;
    std::uint64_t checksum = 0, trades = 0, events = 0;
};

inline RunResult throughput_run(const std::vector<Command>& flow) {
    ChecksumSink sink;
    Book<ChecksumSink> book(sink, flow.size() + 16);
    const auto t0 = Clock::now();
    for (const Command& c : flow) apply_one(book, c);
    const auto t1 = Clock::now();
    return {std::chrono::duration<double>(t1 - t0).count(), sink.hash, sink.trades, sink.events};
}

struct Bucket {
    double lo_ns, hi_ns;  // [lo, hi); hi < 0 means no upper edge
    std::uint64_t count;
};

struct LatencyResult {
    double p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0, mean = 0;
    std::vector<Bucket> buckets;
};

// `step_ns` is the timer's measured resolution: every reading is a whole number of steps,
// so the buckets are centred on whole steps (0, 1, 2, 3 steps, then wider).
inline LatencyResult latency_pass(const std::vector<Command>& flow, double step_ns) {
    ChecksumSink sink;
    Book<ChecksumSink> book(sink, flow.size() + 16);
    std::vector<double> ns(flow.size());
    for (std::size_t i = 0; i < flow.size(); ++i) {
        const auto a = Clock::now();
        apply_one(book, flow[i]);
        const auto b = Clock::now();
        ns[i] = std::chrono::duration<double, std::nano>(b - a).count();
    }
    LatencyResult r;
    double sum = 0;
    for (double v : ns) sum += v;
    r.mean = ns.empty() ? 0 : sum / static_cast<double>(ns.size());
    std::vector<double> sorted = ns;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double q) {
        if (sorted.empty()) return 0.0;
        const std::size_t i = std::min(sorted.size() - 1, static_cast<std::size_t>(q * static_cast<double>(sorted.size())));
        return sorted[i];
    };
    r.p50 = pct(0.50);
    r.p90 = pct(0.90);
    r.p99 = pct(0.99);
    r.p999 = pct(0.999);
    r.max = sorted.empty() ? 0 : sorted.back();
    std::vector<double> edges{0};
    for (double s : {0.5, 1.5, 2.5, 3.5, 5.5, 8.5, 12.5, 24.5, 48.5, 120.5, 1200.5}) edges.push_back(s * step_ns);
    for (std::size_t i = 0; i < edges.size(); ++i)
        r.buckets.push_back({edges[i], i + 1 < edges.size() ? edges[i + 1] : -1.0, 0});
    for (double v : ns) {
        std::size_t b = edges.size() - 1;
        for (std::size_t i = 1; i < edges.size(); ++i)
            if (v < edges[i]) {
                b = i - 1;
                break;
            }
        ++r.buckets[b].count;
    }
    return r;
}

inline double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    if (v.empty()) return 0;
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

inline std::string fixed(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

inline std::string hex64(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
    return buf;
}

inline std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

// "1234567" orders/sec -> "1.23 M"
inline std::string millions(double v) { return fixed(v / 1e6, 2) + " M"; }

inline std::string bucket_label(const Bucket& b) {
    auto f = [](double ns) {
        if (ns >= 1000) return fixed(ns / 1000, ns >= 10000 ? 0 : 1) + " us";
        return fixed(ns, 0) + " ns";
    };
    if (b.hi_ns < 0) return ">= " + f(b.lo_ns);
    return f(b.lo_ns) + " - " + f(b.hi_ns);
}

}  // namespace me
