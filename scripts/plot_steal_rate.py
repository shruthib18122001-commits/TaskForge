#!/usr/bin/env python3
"""Steal-rate instrumentation (Stage 7).

    python scripts/plot_steal_rate.py --csv results/csv/steal_rate.csv \
                                      --out results/charts/steal_rate.png

`taskforge_bench --bench steal-rate` seeds every task onto one worker's deque
and lets the rest steal, sweeping the leaf-duration imbalance factor. Per-worker
counter rows; schema:
  bench,pool,threads,imbalance,worker,wall_ms,tasks_executed,own_pops,
  steals_success,steals_abort,steals_empty,inject_takes,spin_iters,
  yield_iters,sleeps,sleep_ns

Left:  how hard the pool worked to rebalance vs imbalance --
       successful steals / task and total steal attempts / task.
Middle: idle time (sum of per-worker condvar sleep) vs imbalance -- the load
       stealing could NOT hide. Rises as the distribution gets nastier.
Right: per-worker successful-steal counts at the highest imbalance -- the skew
       (light-task grabbers come back for more) is the mechanism visible.
"""

from _common import base_parser, finish, load, plt


def main():
    ap = base_parser(__doc__)
    args = ap.parse_args()
    df = load(args.csv)

    need = {"imbalance", "worker", "steals_success", "steals_abort",
            "steals_empty", "tasks_executed", "sleep_ns"}
    missing = need - set(df.columns)
    if missing:
        raise SystemExit(f"CSV missing columns {missing}; run "
                         "`taskforge_bench --bench steal-rate`")

    by_imb = df.groupby("imbalance").agg(
        tasks=("tasks_executed", "sum"),
        ok=("steals_success", "sum"),
        ab=("steals_abort", "sum"),
        em=("steals_empty", "sum"),
        sleep_ns=("sleep_ns", "sum"),
    ).reset_index()
    by_imb["ok_per_task"] = by_imb["ok"] / by_imb["tasks"].clip(lower=1)
    by_imb["attempts_per_task"] = (
        (by_imb["ok"] + by_imb["ab"] + by_imb["em"]) / by_imb["tasks"].clip(lower=1)
    )
    by_imb["sleep_ms"] = by_imb["sleep_ns"] / 1e6

    fig, (ax1, ax2, ax3) = plt.subplots(1, 3, figsize=(14, 4.4))

    ax1.plot(by_imb["imbalance"], by_imb["ok_per_task"], marker="o", label="successful / task")
    ax1.plot(by_imb["imbalance"], by_imb["attempts_per_task"], marker="s", label="attempts / task")
    ax1.set_xscale("log", base=2)
    ax1.set_xlabel("leaf-duration imbalance (p99 / median)")
    ax1.set_ylabel("steals per task executed")
    ax1.set_title("Rebalancing effort")
    ax1.grid(alpha=0.3, which="both")
    ax1.legend()

    ax2.plot(by_imb["imbalance"], by_imb["sleep_ms"], marker="o", color="C3")
    ax2.set_xscale("log", base=2)
    ax2.set_xlabel("leaf-duration imbalance (p99 / median)")
    ax2.set_ylabel("total worker idle time (ms)")
    ax2.set_title("Load stealing could not hide")
    ax2.grid(alpha=0.3, which="both")

    hi = df[df["imbalance"] == df["imbalance"].max()].sort_values("worker")
    ax3.bar(hi["worker"], hi["steals_success"], color="C0")
    mean = hi["steals_success"].mean()
    ax3.axhline(mean, ls="--", color="grey", label=f"mean {mean:.0f}")
    ax3.set_xlabel("worker")
    ax3.set_ylabel("successful steals")
    ax3.set_title(f"Per-worker steals @ imbalance {df['imbalance'].max():g}")
    ax3.legend()
    ax3.grid(alpha=0.3, axis="y")

    finish(fig, args.out, args.title or "Steal-rate instrumentation")


if __name__ == "__main__":
    main()
