// A small test runner written for this project (no third-party test framework).
//   TEST(name) { ... }            registers a test case
//   CHECK(cond)                    records a failure and goes on
//   CHECK_EQ(a, b)                 the same, printing both values (needs to_string or <<)
//   REQUIRE(cond)                  stops the current test case on failure
// Each test binary includes this header once; it defines main(), which runs every test,
// prints a summary and exits 1 if anything failed. Pass a name to run only matching tests.
#pragma once

#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace testlib {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}
inline int& checks() {
    static int c = 0;
    return c;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct Abort {};

template <class T>
std::string show(const T& v) {
    if constexpr (requires { to_string(v); }) {
        return to_string(v);
    } else if constexpr (requires(std::ostream& os) { os << v; }) {
        std::ostringstream os;
        os << v;
        return os.str();
    } else if constexpr (requires { v.size(); v.begin(); }) {
        std::string s = "[";
        bool first = true;
        for (const auto& x : v) {
            s += (first ? "" : ", ") + show(x);
            first = false;
        }
        return s + "]";
    } else {
        return "<value>";
    }
}

inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, what.c_str());
}

}  // namespace testlib

#define TL_CAT2(a, b) a##b
#define TL_CAT(a, b) TL_CAT2(a, b)
#define TEST(name)                                                                       \
    static void TL_CAT(tl_test_, __LINE__)();                                            \
    static testlib::Register TL_CAT(tl_reg_, __LINE__)(name, TL_CAT(tl_test_, __LINE__)); \
    static void TL_CAT(tl_test_, __LINE__)()

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++testlib::checks();                                               \
        if (!(cond)) testlib::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define REQUIRE(cond)                                                            \
    do {                                                                         \
        ++testlib::checks();                                                     \
        if (!(cond)) {                                                           \
            testlib::fail(__FILE__, __LINE__, "REQUIRE(" #cond ")");             \
            throw testlib::Abort{};                                              \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                                         \
    do {                                                                                       \
        ++testlib::checks();                                                                   \
        const auto& tl_a = (a);                                                                \
        const auto& tl_b = (b);                                                                \
        if (!(tl_a == tl_b))                                                                   \
            testlib::fail(__FILE__, __LINE__,                                                  \
                          "CHECK_EQ(" #a ", " #b ")\n      left:  " + testlib::show(tl_a) +    \
                              "\n      right: " + testlib::show(tl_b));                        \
    } while (0)

int main(int argc, char** argv) {
    int ran = 0, failed_cases = 0;
    for (const auto& c : testlib::registry()) {
        if (argc > 1 && std::strstr(c.name, argv[1]) == nullptr) continue;
        ++ran;
        const int before = testlib::failures();
        try {
            c.fn();
        } catch (const testlib::Abort&) {
        } catch (const std::exception& e) {
            testlib::fail(__FILE__, __LINE__, std::string("exception: ") + e.what());
        }
        const bool ok = testlib::failures() == before;
        if (!ok) ++failed_cases;
        std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", c.name);
    }
    std::printf("%d test case(s), %d check(s), %d failed case(s)\n", ran, testlib::checks(), failed_cases);
    if (ran == 0) {
        std::fprintf(stderr, "no test cases ran\n");
        return 1;
    }
    return failed_cases == 0 ? 0 : 1;
}
