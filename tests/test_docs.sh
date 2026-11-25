#!/usr/bin/env bash
# docs (M6, P4): README.md is a one-page research note with "At a glance" first, a design
# note on the data structures, and only measured numbers (each one must match the saved
# output in results/); EXPLAIN.md explains order books to a newcomer.
set -u
cd "$(dirname "$0")/.."

fails=0
checks=0
ok() { checks=$((checks + 1)); echo "  ok    $1"; }
bad() { checks=$((checks + 1)); fails=$((fails + 1)); echo "  FAIL  $1"; }
expect() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

readme=README.md
explain=EXPLAIN.md
expect "README.md exists" '[ -s "$readme" ]'
expect "EXPLAIN.md exists" '[ -s "$explain" ]'

echo "== README: shape"
expect "the first section is '## At a glance'" '[ "$(grep -m1 "^## " "$readme")" = "## At a glance" ]'
for h in "## At a glance" "## Results" "## Design note" "## How it is checked" "## How to run it" "## Limits"; do
  expect "README has '$h'" 'grep -qx -- "$h" "$readme"'
done
expect "README is about one page (at most 260 lines)" '[ "$(wc -l < "$readme")" -le 260 ]'

echo "== README: the design note covers each data structure and why"
design="$(awk '/^## Design note$/ { on = 1; next } on && /^## / { exit } on' "$readme" 2> /dev/null)"
expect "the Design note section has text" '[ "$(wc -w <<< "$design")" -ge 200 ]'
for term in "price level" "bitmap" "O(1) cancel" "intrusive" "free list" "hash map" "price band" "reference matcher"; do
  expect "design note mentions '$term'" 'grep -qi -- "$term" <<< "$design"'
done
expect "design note says why (at least 4 'because' or 'so that' reasons, in that section)" \
  '[ "$(grep -Eio "because|so that" <<< "$design" | wc -l)" -ge 4 ]'

echo "== README: every number comes from results/"
for f in results/bench.json results/bench.txt results/latency_histogram.svg results/difftest.txt results/replay.txt; do
  expect "$f exists" '[ -s "$f" ]'
done
expect "README embeds results/latency_histogram.svg" 'grep -q "](results/latency_histogram.svg)" "$readme"'
median="$(jq -r '.orders_per_sec.median / 1e6 * 100 | round / 100' results/bench.json)"
expect "README quotes the median orders/sec from bench.json ($median M)" 'grep -qF -- "$median M orders/sec" "$readme"'
for k in p50 p99 p99.9; do
  v="$(jq -r --arg k "$k" '.latency_ns[$k] | round' results/bench.json)"
  expect "README quotes $k = $v ns from bench.json" 'grep -Eq -- "$k[^0-9|]*\| *$v ns|$k $v ns|$k: $v ns" "$readme"'
done
expect "README quotes the machine from bench.json" 'grep -qF -- "$(jq -r .machine results/bench.json)" "$readme"'
expect "README quotes the compiler from bench.json" 'grep -qF -- "$(jq -r .compiler results/bench.json)" "$readme"'
expect "README quotes the flags from bench.json" 'grep -qF -- "$(jq -r .flags results/bench.json)" "$readme"'
expect "README quotes the checksum from bench.json" 'grep -qF -- "$(jq -r .checksum results/bench.json)" "$readme"'
expect "README quotes the warm-up and run counts" \
  'grep -qF -- "$(jq -r "\"\(.warmup) warm-up + \(.runs) timed\"" results/bench.json)" "$readme"'
diffline="$(grep -m1 "^difftest: " results/difftest.txt)"
expect "README quotes the difftest summary line from results/difftest.txt" 'grep -qF -- "$diffline" "$readme"'
expect "results/difftest.txt shows at least 2,000,000 orders agreeing" \
  'grep -Eq "^difftest: seed [0-9]+: ([2-9]|[0-9]{2,}),[0-9]{3},[0-9]{3} orders, .*agree" results/difftest.txt'
replayline="$(grep -m1 "inventory matches" results/replay.txt)"
expect "README quotes the replay summary from results/replay.txt" 'grep -qF -- "$replayline" "$readme"'
replayspeed="$(grep -m1 -Eo "^replay: [0-9.]+ M orders/sec" results/replay.txt | grep -Eo "[0-9.]+ M orders/sec")"
expect "README quotes the replay speed from results/replay.txt (${replayspeed:-none})" \
  '[ -n "$replayspeed" ] && grep -qF -- "$replayspeed" "$readme"'
expect "results/bench.txt is a Release run" 'grep -q "(Release build)" results/bench.txt'
# Every speed and every time the README quotes must be in a saved output file.
sources=(results/*.txt results/*.json results/*/*.txt results/*/*.json)
figs="$( { grep -Eo "[0-9][0-9,.]* M orders/sec" "$readme"; grep -Eo "[0-9][0-9,.]* (ns|us|µs)\b" "$readme"; } 2> /dev/null | sed -e 's/ orders\/sec$//' | sort -u)"
expect "the README quotes some figures" '[ -n "$figs" ]'
# Anchored on the left, so "2 ns" does not match inside "42 ns" and "4 M" not inside "30.44 M".
while IFS= read -r fig; do
  [ -n "$fig" ] || continue
  fig_re="$(sed -e 's/[.]/\\./g' <<< "$fig")"
  expect "the README figure '$fig' appears in results/" \
    'cat "${sources[@]}" 2> /dev/null | grep -Eq -- "(^|[^0-9.,])${fig_re}"'
done <<< "$figs"

echo "== EXPLAIN.md: for a reader new to order books"
for term in "order book" "bid" "ask" "spread" "price-time priority" "limit order" "market order" "partial fill" \
            "cancel" "modify" "IOC" "FOK" "maker" "taker"; do
  expect "EXPLAIN mentions '$term'" 'grep -qi -- "$term" "$explain"'
done
expect "EXPLAIN is a real explainer (at least 700 words)" '[ "$(wc -w < "$explain")" -ge 700 ]'
expect "EXPLAIN points to the walkthrough command" 'grep -q "me ladder scenarios/walkthrough.txt" "$explain"'
expect "EXPLAIN shows a ladder example" 'grep -Eq "^ *ASK +[0-9]+\.[0-9]{2} \|" "$explain" && grep -Eq "^ *BID +[0-9]+\.[0-9]{2} \|" "$explain"'
# The ladder and event lines in EXPLAIN are real output of the walkthrough.
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1 && cmake --build build/release --target me > /dev/null 2>&1
walk="$(./build/release/me ladder scenarios/walkthrough.txt 2> /dev/null | sed -e 's/^ *//' -e 's/ *$//')"
expect "the walkthrough runs" '[ -n "$walk" ]'
while IFS= read -r l; do
  t="$(sed -e 's/^ *//' -e 's/ *$//' <<< "$l")"
  expect "EXPLAIN line '$t' is real walkthrough output" 'grep -qxF -- "$t" <<< "$walk"'
done < <(grep -E "^ *(ASK|BID) +[0-9]+\.[0-9]{2} \||^ *TRADE [0-9]+ @ " "$explain" 2> /dev/null)

echo "$checks check(s), $fails failed"
[ "$fails" -eq 0 ]
