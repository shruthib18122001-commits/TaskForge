# TaskForge

A lock-free work-stealing task scheduler in C++17, plus a DAG dependency
resolver, plus the benchmark and analysis that says where it wins and where it
does not.

The scheduler is the smaller half. The measured story — a granularity sweep with
a located crossover point, a scaling curve with the knee explained from `perf`
counters, a false-sharing experiment run in both directions, and a deliberately
mis-ordered deque caught by ThreadSanitizer — is the deliverable.

> **Status:** implementation complete, all 10 build stages landed. Numbers in
> this README are marked `<!-- MEASURE -->` until filled from a real run on the
> reference machines (see [Reproducing the numbers](#reproducing-the-numbers)).

---

## Architecture

```
  Submit ─▶ [ DAG resolver ] ─▶ ready tasks ─▶ per-worker deques
                                                  │  │  │
                                             worker threads
                                           (pop own / steal others)
```

- **One Chase–Lev deque per worker.** The owner pushes and pops one end; thieves
  steal from the other. The common case — a worker running its own tasks — never
  touches a cache line another core is writing, which is the whole point.
- **External submissions** go through a shared, mutex-guarded *injection queue*,
  not directly onto a worker's deque: Chase–Lev requires a single owner per
  deque. Tasks spawned *from inside* a running task (fork–join) skip the
  injection queue and push straight onto the current worker's deque.
- **Random victim selection** on a steal. Round-robin is worse under skew; see
  [DESIGN.md](DESIGN.md#victim-selection).
- **Hybrid backoff** for idle workers: bounded spin with a CPU relax hint, then
  `yield`, then block on a condition variable with a timeout backstop.
- **DAG resolver:** tasks declare prerequisites and become schedulable when their
  unresolved-dependency count reaches zero. Cycles are detected and rejected at
  submission by a Kahn topological sort.

Full rationale, and every atomic memory-ordering decision, is in
**[DESIGN.md](DESIGN.md)** — the most important file in the repo.

---

## Layout

| Path | What |
|---|---|
| `include/taskforge/` | the whole (header-only) library |
| `include/taskforge/chase_lev_deque.hpp` | the work-stealing deque |
| `include/taskforge/chase_lev_deque_relaxed.hpp` | Stage 9: the deliberately-wrong variant |
| `include/taskforge/thread_pool.hpp` | work-stealing pool: backoff, shutdown, exception policy |
| `include/taskforge/mutex_queue_pool.hpp` | baseline: one global mutex + `std::queue` |
| `include/taskforge/async_pool.hpp` | baseline: `std::async` per task |
| `include/taskforge/dag.hpp` | DAG resolver + cycle detection |
| `include/taskforge/steal_stats.hpp` | Stage 7: per-worker steal instrumentation |
| `include/taskforge/worker_storage.hpp` | Stage 8: packed vs `alignas(64)` worker state |
| `tests/` | GoogleTest suite (built by CMake) |
| `verify/` | framework-free assert-based checks (built by the Makefile) |
| `bench/` | benchmark harness, emits CSV |
| `scripts/` | Python plotters + `perf` wrappers |
| `results/csv,charts,perf/` | generated data (git-ignored; `sample_*.csv` committed) |

---

## Building

### CMake (canonical — this is what CI runs)

```sh
cmake --preset debug     # or: release | asan | tsan
cmake --build --preset debug -j
ctest --preset debug --output-on-failure
```

Presets write to `build/<preset>/`. `asan` = AddressSanitizer + UBSan,
`tsan` = ThreadSanitizer. GoogleTest is fetched via `FetchContent` (needs
network on first configure) or found on the system if already installed.

### Makefile (no CMake / no network / no GoogleTest)

```sh
make verify         # build + run the verify/ checks (no sanitizer)
make verify-tsan    # the same under ThreadSanitizer   <-- run this on ARM
make verify-asan    # the same under ASan + UBSan
make bench          # just the benchmark binary
make buggy-demo     # Stage 9: run the mis-ordered deque under TSan
```

The Makefile covers everything except `tests/` (which needs GoogleTest). The
`verify/` programs call into the same headers the GoogleTest suite does.

---

## Reproducing the numbers

Reference machines:

- **Linux / x86-64** — <!-- MEASURE: CPU, core/thread count, kernel --> (USC CARC).
- **macOS / Apple Silicon** — Apple M5, 10 cores. Weak memory model; this is
  where relaxed-ordering bugs surface.

```sh
# Full benchmark matrix -> results/csv/*.csv
scripts/run_all.sh

# perf-instrumented runs (Linux) -> results/perf/*.txt
scripts/run_perf.sh scaling

# Regenerate every figure from the CSVs
python scripts/plot_scaling.py      --csv results/csv/scaling.csv      --out results/charts/scaling.png
python scripts/plot_granularity.py  --csv results/csv/granularity.csv  --out results/charts/granularity.png
python scripts/plot_steal_rate.py   --csv results/csv/steal_rate.csv   --out results/charts/steal_rate.png
python scripts/plot_false_sharing.py --csv results/csv/false_sharing.csv --out results/charts/false_sharing.png
```

---

## Results

### Scaling (threads 1 → hardware_concurrency)

<!-- MEASURE: paste results/charts/scaling.png and the table -->
_Scaling is linear to ~<!-- MEASURE -->N threads, then departs because
<!-- MEASURE: bandwidth / coherence traffic / steal-path contention -->; see
[scaling-limit analysis](#scaling-limit-analysis)._

### Task-granularity sweep — the key experiment

<!-- MEASURE: paste results/charts/granularity.png -->
Work stealing wins by <!-- MEASURE -->× on <!-- MEASURE -->µs tasks and
converges to parity with the mutex-queue baseline at a per-task size of about
**<!-- MEASURE --> µs** — the crossover. Above that, scheduling overhead is lost
in the task itself and the deque buys nothing. The honest curve, not the single
fine-grained ratio, is the point.

### Imbalanced workload

<!-- MEASURE --> — stealing's best case; speedup vs both baselines.

### Steal-rate instrumentation (Stage 7)

<!-- MEASURE: paste results/charts/steal_rate.png --> — successful steals, failed
steals, and spin time per worker, plotted against load imbalance.

### False-sharing experiment (Stage 8)

| layout | throughput (tasks/s) | cache-misses (`perf`) |
|---|---|---|
| packed per-worker counters | <!-- MEASURE --> | <!-- MEASURE --> |
| `alignas(64)` per-worker counters | <!-- MEASURE --> | <!-- MEASURE --> |

<!-- MEASURE: note the Apple Silicon 128-byte effective line and whether alignas(128) mattered -->

### The deliberately-wrong ordering (Stage 9)

`chase_lev_deque_relaxed.hpp` drops one release fence in `push`. Result:

- casual single run on x86 — <!-- MEASURE: passes -->
- `make buggy-demo` (TSan) — <!-- MEASURE: paste the data-race report -->
- stress run on Apple M5 — <!-- MEASURE: value-check failure / duplicate item -->

Details and the exact diff from the correct version: [DESIGN.md](DESIGN.md#stage-9).

### Scaling-limit analysis

<!-- MEASURE: correlate the scaling knee with perf counters — IPC drop,
cache-miss rate, context-switch / cpu-migration counts — one paragraph per
candidate cause, with the number that rules it in or out. -->

---

## Comparison to production schedulers

Positioning, not a claim of parity:

- **Intel TBB** — task arenas, range partitioners, a `flow::graph` that is a
  superset of this DAG resolver, and NUMA-aware `task_arena`. TaskForge has one
  global thread set and no partitioners.
- **Go runtime scheduler** — G/M/P model, per-P run queues with work stealing
  (this deque is the same idea), plus preemption, a netpoller, and integration
  with the GC. TaskForge tasks run to completion and cannot be preempted.
- **Java `ForkJoinPool`** — Chase–Lev-style deques, continuation stealing,
  `ManagedBlocker` for blocking tasks, `CountedCompleter` for DAGs. TaskForge
  does child stealing (not continuation stealing) and has no managed-blocking
  escape hatch.

---

## What didn't work

Kept as we went; failed experiments are the most credible part of this.

- <!-- MEASURE / fill from the log as stages land: e.g. "round-robin victim
  selection raised failed-steal rate under skew — switched to random" -->
- <!-- MEASURE: "immediate CV block on empty deque added ~Nµs wake latency to
  fork–join recursion — added a spin phase before blocking" -->

---

## License

MIT (see `LICENSE`).
