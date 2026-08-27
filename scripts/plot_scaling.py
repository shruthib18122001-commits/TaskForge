#!/usr/bin/env python3
"""Scaling curve: throughput (and speedup) vs worker-thread count.

    python scripts/plot_scaling.py --csv results/csv/scaling.csv \
                                   --out results/charts/scaling.png

Expects rows with bench == "scaling" (one workload, thread count swept). Marks,
per pool, the thread count where speedup first drops below 0.8 x linear -- the
knee the README's scaling-limit analysis has to explain.
"""

from _common import POOL_LABELS, base_parser, finish, load, plt, pools_in


def knee(threads, speedup, slack=0.8):
    for t, s in zip(threads, speedup):
        if t >= 2 and s < slack * t:
            return t
    return None


def main():
    ap = base_parser(__doc__)
    args = ap.parse_args()
    df = load(args.csv)
    df = df[df["bench"] == "scaling"] if "bench" in df else df
    if df.empty:
        raise SystemExit("no rows with bench == 'scaling'")

    fig, (ax_thru, ax_spd) = plt.subplots(1, 2, figsize=(11, 4.5))

    for pool in pools_in(df):
        d = df[df["pool"] == pool].sort_values("threads")
        if d.empty:
            continue
        t = d["threads"].to_numpy()
        thru = d["throughput_tasks_per_s"].to_numpy() / 1e6
        ax_thru.plot(t, thru, marker="o", label=POOL_LABELS.get(pool, pool))

        base = d[d["threads"] == d["threads"].min()]["throughput_tasks_per_s"].iloc[0]
        speedup = d["throughput_tasks_per_s"].to_numpy() / base
        line, = ax_spd.plot(t, speedup, marker="o", label=POOL_LABELS.get(pool, pool))

        # Only annotate a knee for pools that actually scale for a while first
        # (async never does; a "knee at 2" there is noise).
        k = knee(t, speedup)
        if k is not None and k >= 3:
            ax_spd.axvline(k, ls=":", lw=1, alpha=0.5, color=line.get_color())
            ax_spd.annotate(f"{POOL_LABELS.get(pool, pool)} knee ~{k}",
                            (k, k * 0.8), fontsize=8, rotation=90, va="top",
                            color=line.get_color())

    ax_thru.set_xlabel("worker threads")
    ax_thru.set_ylabel("throughput (M tasks/s)")
    ax_thru.set_title("Throughput")
    ax_thru.grid(alpha=0.3)
    ax_thru.legend()

    lim = df["threads"].max()
    ax_spd.plot([1, lim], [1, lim], ls="--", color="grey", label="linear")
    ax_spd.set_xlabel("worker threads")
    ax_spd.set_ylabel("speedup vs 1 thread")
    ax_spd.set_title("Speedup")
    ax_spd.grid(alpha=0.3)
    ax_spd.legend()

    wl = df["workload"].iloc[0]
    us = df["task_us"].iloc[0]
    finish(fig, args.out, args.title or f"Scaling — {wl}, ~{us:g}us tasks")


if __name__ == "__main__":
    main()
