#!/usr/bin/env bash
# perf-instrumented benchmark runs. Linux only (needs `perf`); on other
# platforms it explains what to run on the Linux box and exits 0 so it can sit
# in a pipeline harmlessly.
#
#   scripts/run_perf.sh <granularity|scaling|imbalanced|dag> [extra bench args...]
#
# Writes:
#   results/perf/<bench>.perf-stat.txt   counter summary
#   results/csv/perf_<bench>.csv         the bench CSV from the same run
#
# The counters (spec): cache-misses, cache-references, context-switches,
# cpu-migrations, instructions, cycles. analyze.py joins these with the scaling
# curve so every performance claim in the README has evidence.
set -euo pipefail

BENCH_NAME="${1:-scaling}"; shift || true

if [[ "$(uname -s)" != "Linux" ]]; then
  cat >&2 <<EOF
run_perf.sh: perf is Linux-only; this host is $(uname -s).
On the Linux reference box:
  scripts/run_perf.sh ${BENCH_NAME} $*
EOF
  exit 0
fi
command -v perf >/dev/null || { echo "perf not installed (linux-tools)"; exit 1; }

BIN="${BENCH:-}"
if [[ -z "$BIN" ]]; then
  for c in build/release/bench/taskforge_bench build/*/bench/taskforge_bench build/debug/taskforge_bench; do
    [[ -x "$c" ]] && BIN="$c" && break
  done
fi
[[ -x "${BIN:-}" ]] || { echo "no taskforge_bench found" >&2; exit 1; }

mkdir -p results/perf results/csv
CSV="results/csv/perf_${BENCH_NAME}.csv"
STAT="results/perf/${BENCH_NAME}.perf-stat.txt"
rm -f "$CSV" "$STAT"

EVENTS=cache-misses,cache-references,context-switches,cpu-migrations,instructions,cycles

echo "perf stat -> $STAT"
perf stat -e "$EVENTS" -o "$STAT" -- \
  "$BIN" --bench "$BENCH_NAME" --pool workstealing --reps 15 --warmup 4 --out "$CSV" "$@"

echo
echo "wrote $STAT and $CSV"
echo "optional deeper look:"
echo "  perf record -g -- $BIN --bench $BENCH_NAME --pool workstealing --reps 5 --out /dev/null"
echo "  perf report --stdio | head -50"
