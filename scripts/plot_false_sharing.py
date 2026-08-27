#!/usr/bin/env python3
"""False-sharing experiment (Stage 8).

    python scripts/plot_false_sharing.py --csv results/csv/false_sharing.csv \
                                         --out results/charts/false_sharing.png

Reads `taskforge_bench --bench false-sharing`, which runs the same workload with
per-worker counters packed (adjacent, sharing cache lines) vs alignas(64)
(one per line), sweeping thread count. Schema adds: layout (packed|aligned).
If a `cache_misses` column is present (from perf, joined in), it is plotted too.
"""

from _common import base_parser, finish, load, plt


def main():
    ap = base_parser(__doc__)
    args = ap.parse_args()
    df = load(args.csv)
    if "layout" not in df.columns:
        raise SystemExit("CSV needs a 'layout' column; run "
                         "`taskforge_bench --bench false-sharing`")

    have_misses = "cache_misses" in df.columns
    ncols = 2 if have_misses else 1
    fig, axes = plt.subplots(1, ncols, figsize=(5.5 * ncols, 4.5), squeeze=False)
    ax = axes[0][0]

    for layout, style in (("packed", dict(marker="o")),
                          ("aligned", dict(marker="s"))):
        d = df[df["layout"] == layout].sort_values("threads")
        if d.empty:
            continue
        ax.plot(d["threads"], d["throughput_tasks_per_s"] / 1e6, label=layout, **style)
    ax.set_xlabel("worker threads")
    ax.set_ylabel("throughput (M tasks/s)")
    ax.set_title("Throughput: packed vs alignas(64)")
    ax.legend()
    ax.grid(alpha=0.3)

    if have_misses:
        ax2 = axes[0][1]
        for layout, style in (("packed", dict(marker="o")),
                              ("aligned", dict(marker="s"))):
            d = df[df["layout"] == layout].sort_values("threads")
            if d.empty:
                continue
            ax2.plot(d["threads"], d["cache_misses"], label=layout, **style)
        ax2.set_xlabel("worker threads")
        ax2.set_ylabel("cache misses (perf)")
        ax2.set_title("Cache misses")
        ax2.legend()
        ax2.grid(alpha=0.3)

    finish(fig, args.out, args.title or "False-sharing experiment")


if __name__ == "__main__":
    main()
