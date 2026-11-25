# Decisions

Design decisions and the alternatives considered, one entry each.

## D3: Price band of 1 to 131071 ticks with directly indexed price levels
Why: indexing a level by its price tick makes finding a level O(1) with no tree, and a
bitmap of non-empty levels finds the next best price in a few word scans. 131071 ticks
($1,310.71 at $0.01) covers the synthetic flow and market-maker-sim's prices (~$100) and
keeps both sides' level arrays near 3 MB. Out-of-band prices are rejected, the same way
in the engine and the reference.
Alternatives: `std::map<price, level>` (O(log n) per new level, pointer chasing); a
sliding window re-centred on the mid (more code, no gain for one symbol).

## D4: Modify always loses queue priority
Why: the design treats modify as "cancel-replace, losing queue priority". Some venues keep priority
on a pure quantity decrease; the engine does not, so the rule is one line and the same in
the reference.
Alternatives: keep priority on quantity decrease (matches some venues, but not this design).

## D5: Latency comes from a separate timestamped pass; throughput from untimed runs
Why: on Apple silicon `steady_clock` ticks at 24 MHz (about 41.7 ns), close to the cost
of one order, so timing every order slows the run and quantizes the result. Throughput is
measured with no per-order timer; per-order latency comes from a separate pass, and the
report prints the measured timer resolution and per-call overhead so a reader can judge
the low percentiles.
Alternatives: time batches of orders (hides the tail the p99.9 is for); kperf cycle
counters (need root).

## D9: Latency readings below one timer step are printed as "< 41 ns", not "0 ns"
Why: most orders take less than one 41.7 ns step (the throughput runs give the exact
mean, about 17-35 ns per order depending on flow size), so the per-order p50 often reads
0 steps. Printing "0 ns" would be a made-up number. The table prints such a value as
"< <step> ns (below 1 timer step)", adds the exact mean ns per order from the throughput
runs, and the histogram's buckets are centred on whole timer steps (0, 1, 2, 3 steps,
then wider), so each bar holds readings that really look alike.
Alternatives: subtract the timer overhead (turns quantized readings into fake precision);
round-number ns buckets (merge 0 and 1 step in one bar).

## D7: A rejected order does not use up its id
Why: a sender whose order was rejected (bad quantity, price out of range, FOK not
fillable) can fix it and resend with the same id; only orders the engine accepted count as
"past orders" for the duplicate-id rule. Both matchers follow the same rule.
Alternatives: every id ever sent is burned (stricter, but punishes a typo).

## D8: A bad modify is rejected before the old order is touched
Why: a modify with quantity 0 or a price outside the band is checked first and rejected,
and the resting order stays exactly as it was (no Cancelled event). A sender's typo
should not silently pull their order. The reference matcher checks in the same order:
unknown id, then quantity, then price.
Alternatives: cancel first, then reject the new half (the order would vanish on a typo).

## D6: Replay uses market-maker-sim's fills CSVs, not its Python simulator
Why: the simulator needs numpy (an install here) and lives in a read-only project. Each
fill row becomes the market maker's 1-lot quote resting at the fill price and the
trader's 1-lot IOC that takes it; the engine's trades must match the CSV prices and the
market maker's inventory column row by row, which checks the mapping as well as the speed.
Alternatives: re-implement the simulator's flow in C++ (a second copy of someone else's
model to keep in sync).
