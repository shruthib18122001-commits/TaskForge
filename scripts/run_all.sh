#!/usr/bin/env bash
# Full benchmark matrix -> results/csv/*.csv
#
#   scripts/run_all.sh [path-to-taskforge_bench]
#
# Defaults to build/release/bench/taskforge_bench (CMake) then
# build/debug/taskforge_bench (Makefile). Override with $1 or $BENCH.
#
# Re-run the plotters afterwards (see README "Reproducing the numbers").
set -euo pipefail

BENCH="${1:-${BENCH:-}}"
if [[ -z "${BENCH}" ]]; then
  for c in build/release/bench/taskforge_bench build/*/bench/taskforge_bench \
           build/debug/taskforge_bench; do
    [[ -x "$c" ]] && BENCH="$c" && break
  done
fi
[[ -x "${BENCH:-}" ]] || { echo "no taskforge_bench found; build it or pass the path" >&2; exit 1; }

OUT=results/csv
mkdir -p "$OUT"
echo "bench binary : $BENCH"
echo "output dir   : $OUT"

REPS="${REPS:-15}"
WARMUP="${WARMUP:-4}"
PIN=""
[[ "${PIN_THREADS:-0}" == "1" ]] && PIN="--pin"

run() {
  local name="$1"; shift
  local csv="$OUT/${name}.csv"
  echo "==> $name -> $csv"
  rm -f "$csv"
  "$BENCH" --reps "$REPS" --warmup "$WARMUP" $PIN "$@" --out "$csv"
}

run granularity --bench granularity --pool all
run scaling     --bench scaling     --pool all --task-us 8 --tasks 200000
run imbalanced  --bench imbalanced  --pool all --task-us 20 --tasks 100000 --imbalance 32
run dag         --bench dag         --pool all
run steal_rate  --bench steal-rate
run false_sharing --bench false-sharing

echo
echo "done. regenerate figures:"
echo "  python scripts/plot_scaling.py       --csv $OUT/scaling.csv       --out results/charts/scaling.png"
echo "  python scripts/plot_granularity.py   --csv $OUT/granularity.csv   --out results/charts/granularity.png"
echo "  python scripts/plot_steal_rate.py    --csv $OUT/steal_rate.csv    --out results/charts/steal_rate.png"
echo "  python scripts/plot_false_sharing.py --csv $OUT/false_sharing.csv --out results/charts/false_sharing.png"
