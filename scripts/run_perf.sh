#!/usr/bin/env bash
# perf-instrumented benchmark runs. Linux only (needs `perf`); on other
# platforms it prints what to run on the Linux box and exits 0.
#
#   scripts/run_perf.sh <scaling|granularity|imbalanced|dag|false-sharing> [args...]
#
# For `scaling` it captures one perf-stat per thread count so analyze.py can
# line each throughput point up with its counters. For the others it captures a
# single perf-stat over the whole run.
#
# Writes:
#   results/perf/<bench>[_tN].perf.csv     machine-readable `perf stat -x,`
#   results/csv/perf_<bench>.csv           the bench's own CSV from the same run
#
# Counters (spec): cache-misses, cache-references, context-switches,
# cpu-migrations, instructions, cycles.
set -euo pipefail

BENCH_NAME="${1:-scaling}"; shift || true

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "run_perf.sh: perf is Linux-only; this host is $(uname -s)." >&2
  echo "On the Linux reference box:  scripts/run_perf.sh ${BENCH_NAME} $*" >&2
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
EVENTS=cache-misses,cache-references,context-switches,cpu-migrations,instructions,cycles
REPS="${REPS:-15}"; WARMUP="${WARMUP:-4}"
NPROC="$(nproc)"

perfrun() {  # <out.perf.csv> -- <bench command...>
  local out="$1"; shift; shift  # drop the "--"
  rm -f "$out"
  perf stat -x, -o "$out" -e "$EVENTS" -- "$@"
}

if [[ "$BENCH_NAME" == "scaling" ]]; then
  CSV="results/csv/perf_scaling.csv"; rm -f "$CSV"
  for t in $(seq 1 "$NPROC"); do
    echo "==> scaling, $t threads"
    perfrun "results/perf/scaling_t${t}.perf.csv" -- \
      "$BIN" --bench scaling --pool workstealing --threads "$t" \
             --reps "$REPS" --warmup "$WARMUP" --out "$CSV" --no-header "$@"
  done
  # header for the concatenated CSV
  "$BIN" --bench scaling --pool workstealing --threads 1 --reps 1 --warmup 0 \
         --out /dev/null >/dev/null 2>&1 || true
  echo "counters -> results/perf/scaling_t*.perf.csv ; curve -> $CSV"
  echo "join them:  python scripts/analyze.py"
else
  CSV="results/csv/perf_${BENCH_NAME}.csv"; rm -f "$CSV"
  perfrun "results/perf/${BENCH_NAME}.perf.csv" -- \
    "$BIN" --bench "$BENCH_NAME" --pool workstealing --reps "$REPS" --warmup "$WARMUP" \
           --out "$CSV" "$@"
  echo "wrote results/perf/${BENCH_NAME}.perf.csv and $CSV"
fi

echo
echo "deeper look (optional):"
echo "  perf record -g -- $BIN --bench $BENCH_NAME --pool workstealing --reps 5 --out /dev/null"
echo "  perf report --stdio | head -50"
