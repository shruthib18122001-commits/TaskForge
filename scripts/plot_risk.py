#!/usr/bin/env python3
"""Parallel risk engine (Phase 2 / P2-5).

    python scripts/plot_risk.py --csv results/csv/sample_risk.csv \
                                --out results/charts/risk.png

`taskforge_bench --bench risk` schema (one schema, two sweeps distinguished
by the `variant` column -- see bench/risk_bench.hpp):
  bench,variant,pool,threads,n_scenarios,chunk_size,n_chunks,reps,
  median_ms,mean_ms,p95_ms,p99_ms,stddev_ms,min_ms,max_ms,
  paths_per_sec,speedup,efficiency,serial_ms,pinned

Four panels:
  top-left:     speedup vs threads, one line per pool (variant == "scaling"),
                with an ideal-linear reference line.
  top-right:    efficiency (speedup / threads) vs threads, same rows -- where
                it crosses below ~0.8 is "the knee" the README calls out.
  bottom-left:  paths/sec vs chunk size, log-x (variant == "chunksize") --
                the scheduling-overhead-vs-parallelism-granularity trade-off;
                a single chunk (far right) has zero parallelism regardless of
                thread count, a tiny chunk (far left) pays per-chunk
                scheduling overhead.
  bottom-right: Amdahl's law. `median_ms` in the scaling rows is ONLY the
                parallel chunk-simulation time (run_parallel never sorts);
                `serial_ms` is the thread-count-independent time to sort and
                reduce the losses array into a VaR/ES, measured once. Total
                wall time is their sum; the dashed curve is what Amdahl's
                law predicts from thread=1's parallel time and that fixed
                serial floor -- the gap between "observed" and "predicted"
                is overhead the simple model doesn't capture (scheduling,
                contention), and the dotted floor is what no amount of
                threads can go below.
"""

from _common import base_parser, finish, load, plt, POOL_LABELS, POOL_ORDER


def _pools_present(df):
    present = [p for p in POOL_ORDER if p in set(df["pool"])]
    return present or sorted(set(df["pool"]))


def main():
    ap = base_parser(__doc__)
    args = ap.parse_args()
    df = load(args.csv)

    need = {"variant", "pool", "threads", "speedup", "efficiency", "chunk_size",
            "paths_per_sec", "median_ms", "serial_ms"}
    missing = need - set(df.columns)
    if missing:
        raise SystemExit(f"CSV missing columns {missing}; run `taskforge_bench --bench risk`")

    scaling = df[df["variant"] == "scaling"]
    chunksize = df[df["variant"] == "chunksize"]
    if scaling.empty or chunksize.empty:
        raise SystemExit("CSV has no 'scaling' and/or 'chunksize' rows -- "
                         "run `taskforge_bench --bench risk` (both sweeps run every time)")

    fig, ((ax1, ax2), (ax3, ax4)) = plt.subplots(2, 2, figsize=(12, 9))

    scaling_pools = _pools_present(scaling)
    for pool in scaling_pools:
        sub = scaling[scaling["pool"] == pool].sort_values("threads")
        label = POOL_LABELS.get(pool, pool)
        ax1.plot(sub["threads"], sub["speedup"], marker="o", label=label)
        ax2.plot(sub["threads"], sub["efficiency"], marker="o", label=label)

    max_t = scaling["threads"].max()
    ax1.plot([1, max_t], [1, max_t], ls="--", color="grey", label="ideal")
    ax1.set_xlabel("threads")
    ax1.set_ylabel("speedup vs 1 thread")
    ax1.set_title("Thread-count scaling")
    ax1.legend()
    ax1.grid(alpha=0.3)

    ax2.axhline(1.0, ls="--", color="grey", label="ideal")
    ax2.set_xlabel("threads")
    ax2.set_ylabel("efficiency (speedup / threads)")
    ax2.set_title("Scaling efficiency")
    ax2.legend()
    ax2.grid(alpha=0.3)

    for pool in _pools_present(chunksize):
        sub = chunksize[chunksize["pool"] == pool].sort_values("chunk_size")
        ax3.plot(sub["chunk_size"], sub["paths_per_sec"], marker="o",
                 label=POOL_LABELS.get(pool, pool))
    ax3.set_xscale("log")
    ax3.set_xlabel("chunk size (scenarios)")
    ax3.set_ylabel("paths/sec")
    ax3.set_title("Chunk-size sweep")
    ax3.legend()
    ax3.grid(alpha=0.3, which="both")

    amdahl_pool = "workstealing" if "workstealing" in set(scaling["pool"]) else scaling_pools[0]
    sub = scaling[scaling["pool"] == amdahl_pool].sort_values("threads")
    serial_ms = float(sub["serial_ms"].iloc[0])
    parallel_1 = float(sub[sub["threads"] == sub["threads"].min()]["median_ms"].iloc[0])
    observed_total = sub["median_ms"] + serial_ms
    predicted_total = serial_ms + parallel_1 / sub["threads"]
    ax4.plot(sub["threads"], observed_total, marker="o", label="observed total")
    ax4.plot(sub["threads"], predicted_total, ls="--", label="Amdahl prediction")
    ax4.axhline(serial_ms, ls=":", color="grey", label=f"serial floor ({serial_ms:.2f} ms)")
    ax4.set_xlabel("threads")
    ax4.set_ylabel("total wall time (ms)")
    ax4.set_title(f"Amdahl's law ({POOL_LABELS.get(amdahl_pool, amdahl_pool)})")
    ax4.legend()
    ax4.grid(alpha=0.3)

    finish(fig, args.out, args.title or "Parallel risk engine")


if __name__ == "__main__":
    main()
