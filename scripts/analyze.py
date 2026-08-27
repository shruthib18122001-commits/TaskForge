#!/usr/bin/env python3
"""Join the scaling curve with its per-thread-count perf counters.

    python scripts/analyze.py                       # defaults below
    python scripts/analyze.py --csv results/csv/perf_scaling.csv \
                              --perf-dir results/perf --out results/perf/scaling_analysis.md

For each thread count it lines up:
  * throughput / speedup / efficiency          (from the bench CSV)
  * IPC = instructions / cycles                 (from perf)
  * cache-miss rate = cache-misses / cache-references
  * context-switches, cpu-migrations

Then it flags where speedup departs from linear and prints, for that knee, which
counter moved — the evidence the README's scaling-limit analysis needs.

perf files are `perf stat -x,` output named `scaling_t<N>.perf.csv` (see
run_perf.sh). Runs on Linux output; on macOS you have the CSV but no perf files,
in which case only the curve half is printed.
"""

import argparse
import os
import sys

import pandas as pd


def df_to_md(df):
    """Minimal markdown table -- avoids a hard dependency on `tabulate`."""
    cols = list(df.columns)
    head = "| " + " | ".join(str(c) for c in cols) + " |"
    sep = "| " + " | ".join("---" for _ in cols) + " |"
    body = [
        "| " + " | ".join("" if pd.isna(v) else str(v) for v in row) + " |"
        for row in df.itertuples(index=False, name=None)
    ]
    return "\n".join([head, sep, *body])


def parse_perf_xsv(path):
    """`perf stat -x,` -> {event: value}. Lines: value,unit,event,...  '#' comments."""
    out = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split(",")
                if len(parts) < 3:
                    continue
                raw, _unit, event = parts[0], parts[1], parts[2]
                if raw in ("<not counted>", "<not supported>", ""):
                    continue
                try:
                    out[event.strip()] = float(raw.replace(" ", ""))
                except ValueError:
                    pass
    except FileNotFoundError:
        return {}
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", default="results/csv/perf_scaling.csv")
    ap.add_argument("--perf-dir", default="results/perf")
    ap.add_argument("--out", default="results/perf/scaling_analysis.md")
    ap.add_argument("--knee-slack", type=float, default=0.8,
                    help="speedup below slack*threads counts as the knee")
    args = ap.parse_args()

    if not os.path.exists(args.csv):
        sys.exit(f"no scaling CSV at {args.csv} -- run scripts/run_perf.sh scaling")
    df = pd.read_csv(args.csv)
    df = df[df["bench"] == "scaling"] if "bench" in df else df
    df = df[df["pool"] == "workstealing"] if "pool" in df else df
    df = df.sort_values("threads").reset_index(drop=True)
    if df.empty:
        sys.exit("scaling CSV has no workstealing rows")

    base = df.iloc[0]["throughput_tasks_per_s"]
    rows = []
    for _, r in df.iterrows():
        t = int(r["threads"])
        speedup = r["throughput_tasks_per_s"] / base
        row = {
            "threads": t,
            "Mtasks_per_s": round(r["throughput_tasks_per_s"] / 1e6, 3),
            "speedup": round(speedup, 2),
            "efficiency": round(speedup / t, 2),
            "p99_ms": round(r.get("p99_ms", float("nan")), 3),
        }
        pf = parse_perf_xsv(os.path.join(args.perf_dir, f"scaling_t{t}.perf.csv"))
        if pf:
            cyc = pf.get("cycles")
            ins = pf.get("instructions")
            cm = pf.get("cache-misses")
            cr = pf.get("cache-references")
            row["IPC"] = round(ins / cyc, 3) if cyc else None
            row["cache_miss_rate"] = round(cm / cr, 4) if cr else None
            row["ctx_switches"] = int(pf.get("context-switches", 0))
            row["cpu_migrations"] = int(pf.get("cpu-migrations", 0))
        rows.append(row)

    table = pd.DataFrame(rows)

    # Knee: first thread count (>=2) whose speedup drops below slack * threads.
    knee = None
    for _, r in table.iterrows():
        if r["threads"] >= 2 and r["speedup"] < args.knee_slack * r["threads"]:
            knee = int(r["threads"])
            break

    lines = ["# Scaling-limit analysis", "",
             f"Baseline throughput at 1 thread: {base/1e6:.3f} M tasks/s.", "",
             df_to_md(table), ""]

    if knee is None:
        lines.append(f"Speedup stayed within {args.knee_slack:.0%} of linear across "
                     f"all {int(table['threads'].max())} threads measured.")
    else:
        lines.append(f"## Knee at ~{knee} threads")
        pre = table[table["threads"] == max(1, knee - 1)]
        at = table[table["threads"] == knee]
        if "IPC" in table.columns and not pre.empty and not at.empty:
            def delta(col):
                a, b = pre.iloc[0].get(col), at.iloc[0].get(col)
                if a is None or b is None:
                    return None
                return b - a
            bits = []
            d_ipc = delta("IPC")
            d_cm = delta("cache_miss_rate")
            if d_ipc is not None:
                bits.append(f"IPC {pre.iloc[0]['IPC']} -> {at.iloc[0]['IPC']} "
                            f"({d_ipc:+.3f})")
            if d_cm is not None:
                bits.append(f"cache-miss rate {pre.iloc[0]['cache_miss_rate']} -> "
                            f"{at.iloc[0]['cache_miss_rate']} ({d_cm:+.4f})")
            bits.append(f"cpu-migrations {pre.iloc[0].get('cpu_migrations')} -> "
                        f"{at.iloc[0].get('cpu_migrations')}")
            lines.append("Across the knee: " + "; ".join(str(b) for b in bits) + ".")
            lines.append("")
            lines.append("Read this as: a falling IPC with a rising cache-miss rate "
                         "points at memory-system / coherence pressure; flat IPC with "
                         "rising context-switches / migrations points at scheduler "
                         "overhead or oversubscription; neither moving points at the "
                         "steal path or genuine serial work. Fill the README's "
                         "conclusion from whichever the numbers show.")
        else:
            lines.append("(no perf files found for the pre-knee / knee thread counts; "
                         "run scripts/run_perf.sh scaling on Linux to populate them)")

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"\nwrote {args.out}")


if __name__ == "__main__":
    main()
