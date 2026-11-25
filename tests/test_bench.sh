#!/usr/bin/env bash
# benchmark (M4, P1, P5, S2): ./bench prints the results table with machine, compiler,
# flags, timer, warm-up and run counts and a reproducible checksum, writes bench.json and
# the Midnight latency histogram SVG; me replay runs market-maker-sim's fills.
# Exit 0 only if every check passes.
set -u
cd "$(dirname "$0")/.."

fails=0
checks=0
ok() { checks=$((checks + 1)); echo "  ok    $1"; }
bad() { checks=$((checks + 1)); fails=$((fails + 1)); echo "  FAIL  $1"; }
expect() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== ./bench, 100,000 orders, seed 42, 1 warm-up + 3 runs"
./bench --orders 100000 --seed 42 --warmup 1 --runs 3 --out "$tmp/a" > "$tmp/a.out" 2> "$tmp/a.err"
code=$?
expect "./bench exits 0" '[ "$code" -eq 0 ]'
out="$(cat "$tmp/a.out")"
for word in "machine " "compiler " "flags " "timer " "flow " "checksum " "orders/sec (median)" \
            "latency p50" "latency p90" "latency p99 " "latency p99.9" "latency max"; do
  expect "output names '$word'" 'grep -qF -- "$word" <<< "$out"'
done
expect "machine line names the CPU and OS" 'grep -Eq "^  machine   .+, [0-9]+ cores, [0-9]+ GiB, macOS" <<< "$out"'
expect "compiler line names clang and a version" 'grep -Eq "^  compiler  .*Clang [0-9]+\." <<< "$out"'
expect "flags line shows -O3 and the Release build" 'grep -Eq "^  flags     .*-O3.*-std=c\+\+20.*\(Release build\)" <<< "$out"'
expect "timer line has resolution and overhead in ns" 'grep -Eq "^  timer     .*resolution [0-9.]+ ns, overhead [0-9.]+ ns" <<< "$out"'
expect "flow line has the seed, the count and the mix" 'grep -Eq "^  flow      seed 42, 100,000 orders: .*limit.*cancel" <<< "$out"'
expect "checksum is 0x + 16 hex digits" 'grep -Eq "^  checksum  0x[0-9a-f]{16} " <<< "$out"'
expect "runs line says 1 warm-up + 3 timed" 'grep -Eq "^  runs      1 warm-up \+ 3 timed" <<< "$out"'
expect "a text histogram is printed" '[ "$(grep -Ec "^  [0-9.]+ (ns|us) - [0-9.]+ (ns|us) " <<< "$out")" -ge 5 ]'
expect "bench.txt is the printed report" 'grep -qF "orders/sec (median)" "$tmp/a/bench.txt"'

json="$tmp/a/bench.json"
expect "bench.json is valid JSON" 'jq -e . "$json" > /dev/null'
for key in machine compiler flags seed orders warmup runs checksum; do
  expect "bench.json has $key" 'jq -e --arg k "$key" "has(\$k)" "$json" > /dev/null'
done
expect "bench.json seed/orders/warmup/runs are the ones asked for" \
  '[ "$(jq -c "[.seed, .orders, .warmup, .runs]" "$json")" = "[42,100000,1,3]" ]'
expect "orders_per_sec has median, min, max, min <= median <= max, 3 runs" \
  'jq -e ".orders_per_sec | (.median > 0) and (.min <= .median) and (.median <= .max) and ((.all | length) == 3)" "$json" > /dev/null'
# p50 may honestly read 0: a whole order can take less than one 41.7 ns timer step.
expect "latency_ns has 0 <= p50 <= p90 <= p99 <= p99.9 <= max, with p99.9 > 0" \
  'jq -e ".latency_ns | (.p50 >= 0) and (.p50 <= .p90) and (.p90 <= .p99) and (.p99 <= .[\"p99.9\"]) and (.[\"p99.9\"] <= .max) and (.[\"p99.9\"] > 0)" "$json" > /dev/null'
step="$(grep -Eo "resolution [0-9.]+ ns" <<< "$out" | grep -Eo "[0-9.]+" | head -1)"
step0="$(printf '%.0f' "${step:-0}")"
for row in "p50" "p90" "p99" "p99.9" "max"; do
  expect "the latency $row row is 'N ns' or '< step ns (below 1 timer step)'" \
    'grep -Eq "^  \| latency $row +\| ([0-9]+ ns|< [0-9]+ ns \(below 1 timer step\)) +\|$" <<< "$out"'
done
# Each printed row matches bench.json: a reading below half a step prints as "< step ns".
for k in p50 p99.9 max; do
  v="$(jq -r --arg k "$k" '.latency_ns[$k]' "$json" 2> /dev/null)"
  if [ -n "$v" ] && [ "$v" != null ] && awk -v v="$v" -v s="${step:-0}" 'BEGIN { exit !(v < s / 2) }'; then
    want="< $step0 ns (below 1 timer step)"
  else
    want="$(printf '%.0f' "${v:-0}") ns"
  fi
  expect "the printed $k ($want) matches bench.json" 'grep -Eq "^  \| latency $k +\| $(sed "s/[().]/\\\\&/g" <<< "$want") +\|$" <<< "$out"'
done
med_m="$(jq -r '.orders_per_sec.median / 1e6 * 100 | round / 100 | tostring' "$json" 2> /dev/null)"
expect "the printed median orders/sec matches bench.json ($med_m M)" \
  'grep -Eq "^  \| orders/sec \(median\) +\| $(printf "%.2f" "${med_m:-0}") M +\|$" <<< "$out"'
# The percentiles sit in the histogram bucket where the running count crosses 50% / 99% / 99.9%.
expect "buckets have lo_ns, hi_ns (null for the last), label and count" \
  'jq -e "(.histogram | length) >= 8 and all(.histogram[]; has(\"lo_ns\") and has(\"hi_ns\") and has(\"label\") and has(\"count\"))" "$json" > /dev/null'
for pair in "p50:0.5" "p99:0.99" "p99.9:0.999"; do
  k="${pair%%:*}"; q="${pair##*:}"
  expect "$k falls in the bucket where the running count reaches $q of the orders" \
    'jq -e --arg k "$k" --argjson q "$q" "
       .latency_ns[\$k] as \$v | .orders as \$n
       | [foreach .histogram[] as \$b (0; . + \$b.count; [., \$b])] | map(select(.[0] >= \$q * \$n)) | .[0][1]
       | (\$v >= .lo_ns) and (.hi_ns == null or \$v < .hi_ns)" "$json" > /dev/null'
done
expect "max falls in the last non-empty bucket" \
  'jq -e ".latency_ns.max as \$v | [.histogram[] | select(.count > 0)] | last | (\$v >= .lo_ns) and (.hi_ns == null or \$v < .hi_ns)" "$json" > /dev/null'
expect "ns_per_order_throughput is 1e9 / median orders/sec" \
  'jq -e "(.ns_per_order_throughput - 1e9 / .orders_per_sec.median) | fabs < 0.01" "$json" > /dev/null'
expect "timer has resolution_ns and overhead_ns > 0" \
  'jq -e ".timer | (.resolution_ns > 0) and (.overhead_ns > 0)" "$json" > /dev/null'
expect "histogram buckets count every order" \
  '[ "$(jq "[.histogram[].count] | add" "$json")" = "100000" ]'
expect "bench.json's checksum is 0x + 16 hex digits" 'jq -er ".checksum" "$json" 2> /dev/null | grep -Eqx "0x[0-9a-f]{16}"'
expect "the checksum in bench.json matches the printed one" \
  'grep -Eq "^  checksum  $(jq -r .checksum "$json" 2> /dev/null) " <<< "$out"'

svg="$tmp/a/latency_histogram.svg"
expect "latency_histogram.svg was written" '[ -s "$svg" ]'
expect "the SVG is an <svg> document" 'head -c 4 "$svg" | grep -q "<svg"'
expect "Midnight colorway: background #02070A, bars #0297DE, text #C9D6DF" \
  'grep -q "#02070A" "$svg" && grep -q "#0297DE" "$svg" && grep -q "#C9D6DF" "$svg"'
expect "one bar per non-empty bucket" \
  '[ "$(grep -c "<rect x=" "$svg")" -ge "$(jq "[.histogram[] | select(.count > 0)] | length" "$json")" ]'
expect "the SVG marks p50, p99 and p99.9" 'grep -q "p50 " "$svg" && grep -q "p99 " "$svg" && grep -q "p99.9 " "$svg"'

echo "== reproducibility: the same seed and flags give the same checksum; another seed does not"
./bench --orders 100000 --seed 42 --warmup 0 --runs 2 --out "$tmp/b" > "$tmp/b.out" 2>&1
./bench --orders 100000 --seed 43 --warmup 0 --runs 1 --out "$tmp/c" > "$tmp/c.out" 2>&1
sum_a="$(jq -er .checksum "$tmp/a/bench.json" 2> /dev/null || echo none-a)"
sum_b="$(jq -er .checksum "$tmp/b/bench.json" 2> /dev/null || echo none-b)"
sum_c="$(jq -er .checksum "$tmp/c/bench.json" 2> /dev/null || echo none-c)"
expect "same seed, same checksum" '[ "$sum_a" = "$sum_b" ]'
expect "same seed, same trade and event counts (both > 0)" \
  'jq -e "(.trades > 0) and (.events > 0)" "$tmp/a/bench.json" > /dev/null 2>&1 && [ "$(jq -c "[.trades, .events]" "$tmp/a/bench.json")" = "$(jq -c "[.trades, .events]" "$tmp/b/bench.json")" ]'
expect "another seed, another checksum" '[ "$sum_a" != "$sum_c" ] && [ "${sum_c#none}" = "$sum_c" ]'

./bench --orders 99999 --seed 42 --warmup 0 --runs 1 --out "$tmp/f" > /dev/null 2>&1
sum_f="$(jq -er .checksum "$tmp/f/bench.json" 2> /dev/null || echo none-f)"
expect "same seed, one order fewer: another checksum and another event count" \
  '[ "${sum_f#none}" = "$sum_f" ] && [ "$sum_f" != "$sum_a" ] && [ "$(jq .events "$tmp/f/bench.json")" != "$(jq .events "$tmp/a/bench.json")" ]'

echo "== defaults: ./bench with no options"
./bench --out "$tmp/d" > "$tmp/d.out" 2>&1; code=$?
expect "./bench with defaults exits 0" '[ "$code" -eq 0 ]'
expect "defaults are seed 42, 1,000,000 orders, 1 warm-up, 5 runs" \
  '[ "$(jq -c "[.seed, .orders, .warmup, .runs]" "$tmp/d/bench.json" 2> /dev/null)" = "[42,1000000,1,5]" ]'
expect "the defaults' runs line says 1 warm-up + 5 timed" 'grep -Eq "^  runs      1 warm-up \+ 5 timed" "$tmp/d.out"'

echo "== bad input"
./bench --bogus 1 > "$tmp/e.out" 2> "$tmp/e.err"; code=$?
expect "./bench --bogus exits 2 with one error line" '[ "$code" -eq 2 ] && [ "$(wc -l < "$tmp/e.err")" -eq 1 ] && grep -q "^error: " "$tmp/e.err"'
./build/release/me bench --runs 0 > /dev/null 2> "$tmp/e.err"; code=$?
expect "me bench --runs 0 exits 2 with one error line" '[ "$code" -eq 2 ] && grep -q "^error: bad value .0. for --runs" "$tmp/e.err"'
python3 -I tools/histogram_svg.py "$tmp/nope.json" "$tmp/x.svg" 2> "$tmp/e.err"; code=$?
expect "histogram_svg.py on a missing file exits 2 with one 'error: cannot read' line" \
  '[ "$code" -eq 2 ] && [ "$(wc -l < "$tmp/e.err")" -eq 1 ] && grep -q "^error: cannot read" "$tmp/e.err"'

echo "== me replay"
./build/release/me replay tests/fixtures/fills_sample.csv --runs 5 > "$tmp/r.out" 2> "$tmp/r.err"; code=$?
expect "replay of the fixture exits 0" '[ "$code" -eq 0 ]'
expect "replay summary: 1 file, 40 fills, 80 orders, 40 trades, inventory matches" \
  'grep -qx "replay: 1 file(s), 40 fills, 80 orders, 40 trades, inventory matches" "$tmp/r.out"'
expect "replay reports orders/sec as the median of 5 timed replays" \
  'grep -Eq "^replay: [0-9.]+ M orders/sec \(median of 5 timed replays" "$tmp/r.out"'
./build/release/me replay tests/fixtures/fills_sample.csv > "$tmp/r.out" 2>&1; code=$?
expect "replay without --runs uses 20 timed replays" \
  '[ "$code" -eq 0 ] && grep -Eq "^replay: [0-9.]+ M orders/sec \(median of 20 timed replays" "$tmp/r.out"'
./build/release/me replay tests/fixtures/fills_sample.csv tests/fixtures/fills_sample.csv --runs 1 > "$tmp/r.out" 2>&1; code=$?
expect "two files: inventory starts at 0 for each file" \
  '[ "$code" -eq 0 ] && grep -qx "replay: 2 file(s), 80 fills, 160 orders, 80 trades, inventory matches" "$tmp/r.out"'
# A fills file whose inventory column disagrees with the trades the engine made.
sed -e '4s/,-1,10010,/,5,10010,/' tests/fixtures/fills_sample.csv > "$tmp/badinv.csv"
./build/release/me replay "$tmp/badinv.csv" > "$tmp/r.out" 2>&1; code=$?
expect "a wrong inventory column is a MISMATCH naming the row, exit 1" \
  '[ "$code" -eq 1 ] && grep -q "MISMATCH: .*row 3: inventory -1 after the trade, the CSV says 5" "$tmp/r.out"'
printf 'a,b\n1,2\n' > "$tmp/notfills.csv"
./build/release/me replay "$tmp/notfills.csv" > /dev/null 2> "$tmp/e.err"; code=$?
expect "a CSV without side/price/inventory is one error line, exit 2" \
  '[ "$code" -eq 2 ] && [ "$(wc -l < "$tmp/e.err")" -eq 1 ] && grep -q "^error: .*not a market-maker-sim fills file" "$tmp/e.err"'
./build/release/me replay "$tmp/missing.csv" > /dev/null 2> "$tmp/e.err"; code=$?
expect "a missing file is one error line, exit 2" '[ "$code" -eq 2 ] && grep -q "^error: cannot read" "$tmp/e.err"'
mms="$HOME/Projects/market-maker-sim/results"
if ls "$mms"/fills_*.csv > /dev/null 2>&1; then
  ./build/release/me replay "$mms"/fills_*.csv --runs 3 > "$tmp/r.out" 2>&1; code=$?
  expect "replay of market-maker-sim's real fills: inventory matches" \
    '[ "$code" -eq 0 ] && grep -Eq "^replay: [0-9]+ file\(s\), [0-9,]+ fills, .*inventory matches" "$tmp/r.out"'
else
  echo "  note  market-maker-sim results not found at $mms; real-file replay skipped"
fi

echo "$checks check(s), $fails failed"
[ "$fails" -eq 0 ]
