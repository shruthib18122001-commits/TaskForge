"""Shared helpers for the TaskForge plot scripts.

All plotters share a CSV schema (see bench/bench_common.hpp):

    bench,pool,threads,workload,task_us,n_tasks,reps,
    median_ms,mean_ms,p95_ms,p99_ms,stddev_ms,min_ms,max_ms,
    throughput_tasks_per_s,iters_per_us,pinned

The steal-rate and false-sharing sweeps (Stages 7-8) add their own columns;
those plotters read what they need and ignore the rest.
"""

import argparse
import sys

import matplotlib

matplotlib.use("Agg")  # headless: write files, never open a window
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

POOL_LABELS = {
    "workstealing": "work-stealing",
    "mutex": "mutex queue",
    "async": "std::async",
}
POOL_ORDER = ["workstealing", "mutex", "async"]


def base_parser(description):
    p = argparse.ArgumentParser(description=description)
    p.add_argument("--csv", required=True, help="input CSV from taskforge_bench")
    p.add_argument("--out", required=True, help="output image path (.png/.svg/.pdf)")
    p.add_argument("--title", default=None, help="override the figure title")
    return p


def load(csv_path):
    try:
        df = pd.read_csv(csv_path)
    except FileNotFoundError:
        sys.exit(f"no such CSV: {csv_path}")
    if df.empty:
        sys.exit(f"CSV has no rows: {csv_path}")
    return df


def pools_in(df):
    present = [p for p in POOL_ORDER if p in set(df["pool"])]
    return present or sorted(set(df["pool"]))


def finish(fig, out_path, title):
    if title:
        fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(out_path, dpi=130)
    print(f"wrote {out_path}")
