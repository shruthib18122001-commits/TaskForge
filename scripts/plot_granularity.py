#!/usr/bin/env python3
"""Task-granularity sweep -- the key experiment.

    python scripts/plot_granularity.py --csv results/csv/granularity.csv \
                                       --out results/charts/granularity.png

Left:  throughput vs per-task work (both log axes), one line per pool.
Right: work-stealing speedup over the mutex-queue baseline vs per-task work,
       with the crossover (speedup == 1) marked -- above that per-task size the
       deque buys nothing.
"""

import numpy as np

from _common import POOL_LABELS, base_parser, finish, load, plt, pools_in


def crossover_us(us, ratio):
    """First task size (us) at which ws/mutex throughput ratio drops to 1.0."""
    for i in range(1, len(us)):
        if (ratio[i - 1] - 1.0) * (ratio[i] - 1.0) <= 0 and ratio[i - 1] != ratio[i]:
            # linear interpolation in log-us space
            l0, l1 = np.log(us[i - 1]), np.log(us[i])
            r0, r1 = ratio[i - 1], ratio[i]
            frac = (1.0 - r0) / (r1 - r0)
            return float(np.exp(l0 + frac * (l1 - l0)))
    return None


def main():
    ap = base_parser(__doc__)
    args = ap.parse_args()
    df = load(args.csv)
    df = df[df["bench"] == "granularity"] if "bench" in df else df
    if df.empty:
        raise SystemExit("no rows with bench == 'granularity'")

    fig, (ax_t, ax_r) = plt.subplots(1, 2, figsize=(11, 4.5))

    for pool in pools_in(df):
        d = df[df["pool"] == pool].sort_values("task_us")
        if d.empty:
            continue
        ax_t.plot(d["task_us"], d["throughput_tasks_per_s"] / 1e6, marker="o",
                  label=POOL_LABELS.get(pool, pool))
    ax_t.set_xscale("log")
    ax_t.set_yscale("log")
    ax_t.set_xlabel("per-task work (microseconds)")
    ax_t.set_ylabel("throughput (M tasks/s)")
    ax_t.set_title("Throughput vs granularity")
    ax_t.grid(alpha=0.3, which="both")
    ax_t.legend()

    ws = df[df["pool"] == "workstealing"].sort_values("task_us")
    mx = df[df["pool"] == "mutex"].sort_values("task_us")
    if not ws.empty and not mx.empty:
        merged = ws.merge(mx, on="task_us", suffixes=("_ws", "_mx"))
        us = merged["task_us"].to_numpy()
        ratio = (merged["throughput_tasks_per_s_ws"]
                 / merged["throughput_tasks_per_s_mx"]).to_numpy()
        ax_r.plot(us, ratio, marker="o", color="C0")
        ax_r.axhline(1.0, ls="--", color="grey")
        x = crossover_us(us, ratio)
        if x is not None:
            ax_r.axvline(x, ls=":", color="C3")
            ax_r.annotate(f"crossover ~{x:.2g} us", (x, 1.0),
                          textcoords="offset points", xytext=(6, 10), color="C3")
        ax_r.set_xscale("log")
        ax_r.set_xlabel("per-task work (microseconds)")
        ax_r.set_ylabel("work-stealing throughput / mutex-queue")
        ax_r.set_title("Where the deque stops helping")
        ax_r.grid(alpha=0.3, which="both")
    else:
        ax_r.text(0.5, 0.5, "need both 'workstealing' and 'mutex' rows",
                  ha="center", va="center", transform=ax_r.transAxes)

    finish(fig, args.out, args.title or "Task-granularity sweep")


if __name__ == "__main__":
    main()
