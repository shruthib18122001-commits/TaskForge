#!/usr/bin/env python3
"""Steal-rate instrumentation (Stage 7).

    python scripts/plot_steal_rate.py --csv results/csv/steal_rate.csv \
                                      --out results/charts/steal_rate.png

Reads the per-worker steal counters emitted by `taskforge_bench --bench
steal-rate` (schema: adds imbalance,worker,tasks_executed,steals_success,
steals_abort,steals_empty,spin_iters to the common columns).

Left:  steal rate (successful steals / task) vs configured load imbalance.
Right: per-worker task counts + steal counts at the highest imbalance point.
"""

from _common import base_parser, finish, load, plt


def main():
    ap = base_parser(__doc__)
    args = ap.parse_args()
    df = load(args.csv)

    need = {"imbalance", "worker", "steals_success", "tasks_executed"}
    if not need.issubset(df.columns):
        raise SystemExit(f"CSV missing columns {need - set(df.columns)}; "
                         "run `taskforge_bench --bench steal-rate`")

    fig, (ax_l, ax_r) = plt.subplots(1, 2, figsize=(11, 4.5))

    g = df.groupby("imbalance").agg(
        steals=("steals_success", "sum"),
        tasks=("tasks_executed", "sum"),
    ).reset_index()
    g["rate"] = g["steals"] / g["tasks"].clip(lower=1)
    ax_l.plot(g["imbalance"], g["rate"], marker="o")
    ax_l.set_xlabel("load imbalance (p99 / median task duration)")
    ax_l.set_ylabel("successful steals per task")
    ax_l.set_title("Stealing tracks imbalance")
    ax_l.set_xscale("log")
    ax_l.grid(alpha=0.3, which="both")

    hi = df[df["imbalance"] == df["imbalance"].max()].sort_values("worker")
    x = hi["worker"].to_numpy()
    ax_r.bar(x - 0.2, hi["tasks_executed"], width=0.4, label="tasks executed")
    ax_r.bar(x + 0.2, hi["steals_success"], width=0.4, label="successful steals")
    ax_r.set_xlabel("worker")
    ax_r.set_title(f"Per-worker @ imbalance {df['imbalance'].max():g}")
    ax_r.legend()
    ax_r.grid(alpha=0.3, axis="y")

    finish(fig, args.out, args.title or "Steal-rate instrumentation")


if __name__ == "__main__":
    main()
