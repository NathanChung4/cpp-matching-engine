// ladder-cli (M5, P2, P6): `me ladder` prints the book as a price ladder after every
// order of a scenario; bad input is one `error:` line on stderr and exit 2.
// Runs the real `me` binary (built next to this test).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "testlib.hpp"

namespace {

const std::string kMe = std::string(ME_BINARY_DIR) + "/me";
const std::string kRoot = ME_SOURCE_DIR;

struct Run {
    int code = -1;
    std::string out, err;
};

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string temp_path(const std::string& name) {
    return "/tmp/me_test_cli_" + std::to_string(getpid()) + "_" + name;
}

std::string write_temp(const std::string& name, const std::string& text) {
    const std::string p = temp_path(name);
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

Run run(const std::string& args) {
    const std::string o = temp_path("out"), e = temp_path("err");
    const std::string cmd = "'" + kMe + "' " + args + " > '" + o + "' 2> '" + e + "'";
    const int status = std::system(cmd.c_str());
    Run r;
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    r.out = slurp(o);
    r.err = slurp(e);
    std::remove(o.c_str());
    std::remove(e.c_str());
    return r;
}

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) out.push_back(l);
    return out;
}

bool has_line(const std::string& text, const std::string& line) {
    for (const auto& l : lines(text))
        if (l == line) return true;
    return false;
}

// Line `second` comes right after line `first` somewhere in text.
bool followed_by(const std::string& text, const std::string& first, const std::string& second) {
    const auto ls = lines(text);
    for (std::size_t i = 0; i + 1 < ls.size(); ++i)
        if (ls[i] == first && ls[i + 1] == second) return true;
    return false;
}

// Line `want` appears in the block printed for the command whose header is `header`.
bool in_block(const std::string& text, const std::string& header, const std::string& want) {
    const auto ls = lines(text);
    for (std::size_t i = 0; i < ls.size(); ++i) {
        if (ls[i] != header) continue;
        for (std::size_t k = i + 1; k < ls.size() && !(ls[k].size() > 1 && ls[k][0] == '['); ++k)
            if (ls[k] == want) return true;
    }
    return false;
}

// A scenario error: exit 2, nothing on stdout, exactly the one expected line on stderr.
void expect_error(const std::string& scenario, const std::string& message) {
    const std::string p = write_temp("bad.txt", scenario);
    const Run r = run("ladder '" + p + "'");
    CHECK_EQ(r.code, 2);
    CHECK_EQ(r.out, std::string());
    CHECK_EQ(r.err, message + "\n");
    std::remove(p.c_str());
}

}  // namespace

TEST("ladder: a small scenario prints exactly the expected events and ladders") {
    const std::string p = write_temp("small.txt",
                                     "# comment line\n"
                                     "limit sell 1 100.05 10   # trailing comment\n"
                                     "\n"
                                     "limit  sell 2 100.5 5\n"
                                     "limit buy 3 99 7\n"
                                     "limit buy 4 100.05 12\n"
                                     "cancel 3\n");
    const Run r = run("ladder '" + p + "'");
    CHECK_EQ(r.code, 0);
    CHECK_EQ(r.err, std::string());
    CHECK_EQ(r.out, std::string("[1] limit sell 1 100.05 10\n"
                                "    REST #1 sell 10 @ 100.05\n"
                                "      ASK   100.05 |        10 | #1:10\n"
                                "      -------------- spread (one side empty) --------------\n"
                                "      (no bids)\n"
                                "\n"
                                "[2] limit sell 2 100.5 5\n"
                                "    REST #2 sell 5 @ 100.50\n"
                                "      ASK   100.50 |         5 | #2:5\n"
                                "      ASK   100.05 |        10 | #1:10\n"
                                "      -------------- spread (one side empty) --------------\n"
                                "      (no bids)\n"
                                "\n"
                                "[3] limit buy 3 99 7\n"
                                "    REST #3 buy 7 @ 99.00\n"
                                "      ASK   100.50 |         5 | #2:5\n"
                                "      ASK   100.05 |        10 | #1:10\n"
                                "      -------------- spread 1.05 --------------\n"
                                "      BID    99.00 |         7 | #3:7\n"
                                "\n"
                                "[4] limit buy 4 100.05 12\n"
                                "    TRADE 10 @ 100.05  taker #4  maker #1\n"
                                "    REST #4 buy 2 @ 100.05\n"
                                "      ASK   100.50 |         5 | #2:5\n"
                                "      -------------- spread 0.45 --------------\n"
                                "      BID   100.05 |         2 | #4:2\n"
                                "      BID    99.00 |         7 | #3:7\n"
                                "\n"
                                "[5] cancel 3\n"
                                "    CANCELLED #3 7 left @ 99.00\n"
                                "      ASK   100.50 |         5 | #2:5\n"
                                "      -------------- spread 0.45 --------------\n"
                                "      BID   100.05 |         2 | #4:2\n"
                                "\n"));
    std::remove(p.c_str());
}

TEST("ladder: the walkthrough prints a ladder after every one of its orders") {
    const Run r = run("ladder '" + kRoot + "/scenarios/walkthrough.txt'");
    CHECK_EQ(r.code, 0);
    CHECK_EQ(r.err, std::string());
    std::size_t commands = 0;
    for (const auto& l : lines(slurp(kRoot + "/scenarios/walkthrough.txt"))) {
        const std::string body = l.substr(0, l.find('#'));
        if (body.find_first_not_of(" \t\r") != std::string::npos) ++commands;
    }
    CHECK(commands >= 10);
    std::size_t headers = 0, spreads = 0;
    for (const auto& l : lines(r.out)) {
        if (l.size() > 1 && l[0] == '[') ++headers;
        if (l.find("spread") != std::string::npos) ++spreads;
    }
    CHECK_EQ(headers, commands);
    CHECK_EQ(spreads, commands);
    // The moments the walkthrough is for:
    CHECK(has_line(r.out, "[5] limit buy 5 100.05 12"));
    CHECK(has_line(r.out, "    TRADE 10 @ 100.05  taker #5  maker #1"));  // time priority: #1 first
    CHECK(has_line(r.out, "    TRADE 2 @ 100.05  taker #5  maker #2"));   // partial fill of #2
    CHECK(has_line(r.out, "    TRADE 20 @ 100.07  taker #6  maker #3"));  // sweep to the next level
    CHECK(has_line(r.out, "    REST #6 buy 7 @ 100.07"));
    CHECK(has_line(r.out, "    EXPIRED #8 8 unfilled"));                  // market remainder
    CHECK(has_line(r.out, "    CANCELLED #9 8 left @ 100.10"));
    CHECK(has_line(r.out, "    REJECTED #1: unknown order id"));          // cancel of a filled order
    CHECK(has_line(r.out, "    TRADE 5 @ 100.20  taker #12  maker #11"));  // modify lost #10's priority
    CHECK(has_line(r.out, "    EXPIRED #13 4 unfilled"));                 // IOC remainder
    CHECK(has_line(r.out, "    REJECTED #14: FOK not fillable"));
    CHECK(has_line(r.out, "    TRADE 40 @ 100.30  taker #16  maker #15"));
    // Levels with several orders: the total, and the orders in time order.
    CHECK(in_block(r.out, "[2] limit sell 2 100.05 5", "      ASK   100.05 |        15 | #1:10 #2:5"));
    CHECK(in_block(r.out, "[13] limit sell 11 100.20 5", "      ASK   100.20 |        10 | #10:5 #11:5"));
    CHECK(in_block(r.out, "[14] modify 10 100.20 5", "      ASK   100.20 |        10 | #11:5 #10:5"));
    CHECK(in_block(r.out, "[7] limit buy 7 99.98 10", "      BID   100.07 |         7 | #6:7"));
}

TEST("ladder: levels with several orders on both sides, exactly") {
    const std::string p = write_temp("multi.txt",
                                     "limit buy 1 99.90 3\n"
                                     "limit buy 2 99.90 4\n"
                                     "limit sell 3 100.10 5\n"
                                     "limit sell 4 100.10 6\n"
                                     "modify 1 99.90 3\n");
    const Run r = run("ladder '" + p + "'");
    CHECK_EQ(r.code, 0);
    const auto ls = lines(r.out);
    const std::vector<std::string> tail(ls.end() - std::min<std::size_t>(ls.size(), 7), ls.end());
    CHECK_EQ(tail, (std::vector<std::string>{"[5] modify 1 99.90 3",
                                             "    CANCELLED #1 3 left @ 99.90",
                                             "    REST #1 buy 3 @ 99.90",
                                             "      ASK   100.10 |        11 | #3:5 #4:6",
                                             "      -------------- spread 0.20 --------------",
                                             "      BID    99.90 |         7 | #2:4 #1:3",
                                             ""}));
    std::remove(p.c_str());
}

TEST("errors: unknown subcommand, bad options and missing files are one line and exit 2") {
    Run r = run("frobnicate");
    CHECK_EQ(r.code, 2);
    CHECK_EQ(r.err, std::string("error: unknown command 'frobnicate' (try: me help)\n"));
    r = run("ladder");
    CHECK_EQ(r.code, 2);
    CHECK_EQ(r.err, std::string("error: usage: me ladder <scenario file>\n"));
    r = run("ladder /nonexistent/nope.txt");
    CHECK_EQ(r.code, 2);
    CHECK_EQ(r.err, std::string("error: cannot read '/nonexistent/nope.txt'\n"));
    r = run("difftest --orders x");
    CHECK_EQ(r.code, 2);
    CHECK_EQ(r.err, std::string("error: bad value 'x' for --orders\n"));
    r = run("difftest --frob 1");
    CHECK_EQ(r.code, 2);
    CHECK_EQ(r.err, std::string("error: unknown option '--frob'\n"));
    r = run("difftest --seed");
    CHECK_EQ(r.code, 2);
    CHECK_EQ(r.err, std::string("error: option '--seed' needs a value\n"));
    r = run("help");
    CHECK_EQ(r.code, 0);
    CHECK(r.out.find("ladder") != std::string::npos);
}

TEST("errors: bad scenario lines name the line and stop before any output") {
    expect_error("limit buy 1 100.00 10\nlimit buy x 100.00 10\n", "error: line 2: bad order id 'x'");
    expect_error("limit hold 1 100.00 10\n", "error: line 1: bad side 'hold' (use buy or sell)");
    expect_error("\n\nlimit buy 1 100.001 10\n",
                 "error: line 3: bad price '100.001' (use a number with at most 2 decimals)");
    expect_error("limit buy 1 -5 10\n", "error: line 1: bad price '-5' (use a number with at most 2 decimals)");
    expect_error("limit buy 1 100.00 ten\n", "error: line 1: bad quantity 'ten'");
    expect_error("limit buy 1 100.00\n", "error: line 1: 'limit' needs: <buy|sell> <id> <price> <qty>");
    expect_error("market buy 1\n", "error: line 1: 'market' needs: <buy|sell> <id> <qty>");
    expect_error("cancel\n", "error: line 1: 'cancel' needs: <id>");
    expect_error("buy 1 100 10\n",
                 "error: line 1: unknown command 'buy' (use limit, market, ioc, fok, cancel or modify)");
    expect_error("# only a comment\n", std::string("error: '") + temp_path("bad.txt") + "' has no commands");
}

TEST("errors: cancel or modify of an id the scenario never submitted is a CLI error") {
    expect_error("limit sell 1 100.05 10\ncancel 99\n", "error: line 2: unknown order id 99");
    expect_error("limit sell 1 100.05 10\nmodify 7 100.00 5\n", "error: line 2: unknown order id 7");
    // ...but an id that was submitted and is gone is the engine's REJECTED event, not an error.
    const std::string p = write_temp("gone.txt", "limit sell 1 100.05 10\nmarket buy 2 10\ncancel 1\nmodify 1 100 1\n");
    const Run r = run("ladder '" + p + "'");
    CHECK_EQ(r.code, 0);
    CHECK(followed_by(r.out, "[3] cancel 1", "    REJECTED #1: unknown order id"));
    CHECK(followed_by(r.out, "[4] modify 1 100 1", "    REJECTED #1: unknown order id"));
    std::remove(p.c_str());
}

TEST("engine rejects inside a scenario are events, and the scenario goes on") {
    const std::string p = write_temp("rej.txt",
                                     "limit buy 1 100.00 0\n"
                                     "limit buy 2 2000.00 5\n"
                                     "limit buy 3 100.00 5\n"
                                     "limit buy 3 100.00 5\n"
                                     "market sell 4 2\n");
    const Run r = run("ladder '" + p + "'");
    CHECK_EQ(r.code, 0);
    CHECK(has_line(r.out, "    REJECTED #1: bad quantity"));
    CHECK(has_line(r.out, "    REJECTED #2: price out of range"));
    CHECK(has_line(r.out, "    REJECTED #3: duplicate order id"));
    CHECK(has_line(r.out, "    TRADE 2 @ 100.00  taker #4  maker #3"));
    CHECK(has_line(r.out, "      BID   100.00 |         3 | #3:3"));
    std::remove(p.c_str());
}

TEST("difftest from the command line prints the summary line") {
    const Run r = run("difftest --orders 20000 --seed 3");
    CHECK_EQ(r.code, 0);
    CHECK(r.out.rfind("difftest: seed 3: 20,000 orders, ", 0) == 0);
    CHECK(r.out.find("engine and reference agree on every event and on the final book") != std::string::npos);
}
