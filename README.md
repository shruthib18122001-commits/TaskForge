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
| `include/taskforge/false_sharing.hpp` | Stage 8: packed vs `alignas(64)` counter harness |
| (Stage 7 per-worker counters live in `thread_pool.hpp` — `WorkerCounters` / `stats()`) | |
| `tests/` | GoogleTest suite (built by CMake) |
| `verify/` | framework-free assert-based checks (built by the Makefile) |
| `bench/` | benchmark harness, emits CSV |
| `scripts/` | Python plotters, `perf` wrapper, scaling-limit analyzer |
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
# Full benchmark matrix -> results/csv/*.csv  (granularity, scaling, imbalanced, dag)
scripts/run_all.sh

# Or one sweep at a time (see bench_main.cpp for all options):
build/*/bench/taskforge_bench --bench scaling    --pool all --task-us 8 --out results/csv/scaling.csv
build/*/bench/taskforge_bench --bench granularity --pool all --out results/csv/granularity.csv

# perf-instrumented scaling run (Linux): one perf-stat per thread count
scripts/run_perf.sh scaling
python scripts/analyze.py           # joins perf counters with the curve -> results/perf/scaling_analysis.md

# Regenerate every figure from the CSVs
python scripts/plot_scaling.py       --csv results/csv/scaling.csv       --out results/charts/scaling.png
python scripts/plot_granularity.py   --csv results/csv/granularity.csv   --out results/charts/granularity.png
python scripts/plot_steal_rate.py    --csv results/csv/steal_rate.csv    --out results/charts/steal_rate.png
python scripts/plot_false_sharing.py --csv results/csv/false_sharing.csv --out results/charts/false_sharing.png
```

Every sweep shares one CSV schema (`bench,pool,threads,workload,task_us,n_tasks,
reps,median_ms,…,throughput_tasks_per_s,iters_per_us,pinned`); rows concatenate.
`iters_per_us` is the busy-wait calibration for that run — recorded so a reader
can judge it. `results/csv/sample_*.csv` are committed so CI can smoke-test the
plotters.

---

## Results

### Scaling (threads 1 → hardware_concurrency)

<!-- MEASURE: paste results/charts/scaling.png and the table from scripts/analyze.py -->
On the dev box (Apple M5, 10 cores, 8µs tasks) the work-stealing pool reached
~6.4× at 10 threads with efficiency crossing 0.8 at ~6 threads — the knee. The
mutex-queue baseline tracked it within a few percent for this flat,
externally-fed workload (see the note below); `std::async` did not scale at all.
Reference-machine numbers and the `perf`-backed cause go in
[scaling-limit analysis](#scaling-limit-analysis).

> **Why the mutex baseline keeps up here.** A single external producer feeding
> N independent tasks sends every one of them through the *injection queue* — a
> mutex-guarded global queue — because Chase–Lev forbids a second writer on a
> worker's deque. So this workload never really exercises the deques. The
> work-stealing advantage shows up when tasks live on worker deques: recursive
> fork–join, and imbalanced loads. The granularity and steal-rate sweeps below
> use those.

### Task-granularity sweep — the key experiment

<!-- MEASURE: paste results/charts/granularity.png -->
Work stealing wins by <!-- MEASURE -->× on <!-- MEASURE -->µs tasks and
converges to parity with the mutex-queue baseline at a per-task size of about
**<!-- MEASURE --> µs** — the crossover. Above that, scheduling overhead is lost
in the task itself and the deque buys nothing. `std::async` is <!-- MEASURE -->×
slower on the fine end (a thread per task). The honest curve, not the single
fine-grained ratio, is the point.

### Imbalanced workload

<!-- MEASURE: uniform vs mixed vs heavy-tailed, same total work; speedup vs both
baselines. Heavy-tailed is where stealing should look best. -->

### Steal-rate instrumentation (Stage 7)

<!-- MEASURE: paste results/charts/steal_rate.png -->
The steal-rate sweep seeds every task onto one worker's deque and lets the rest
steal, sweeping the leaf-duration imbalance factor. The signal: as imbalance
rises, total worker idle time (summed condvar sleep, `sleep_ns`) rises
monotonically — the load stealing could *not* hide — and the per-worker steal
counts skew (light-task grabbers come back for more). Invariant checked by the
tests: `own_pops + steals_success + inject_takes == tasks_executed` per worker.

### False-sharing experiment (Stage 8)

`include/taskforge/false_sharing.hpp` isolates the per-worker counter update the
pool does on every task, and runs it two ways while sweeping thread count:
`PackedSlot` (8-byte atomics, no alignment — up to 8 pack into one cache line)
vs `AlignedSlot` (`alignas(64)`, one line each).

Dev machine (Apple M5, 10 cores), 200M increments/thread, `median_ms`:

| threads | packed | aligned | packed ×slower |
|--:|--:|--:|--:|
| 1 | 32 ms | 31 ms | 1.0× |
| 2 | 213 ms | 32 ms | 6.7× |
| 4 | 667 ms | 33 ms | 20× |
| 8 | 3133 ms | 65 ms | 48× |

Linux / `perf` cache-miss counts: <!-- MEASURE: scripts/run_perf.sh false-sharing -->.
Apple Silicon's L2 is documented to work in 128-byte pairs for coherence, so
`-DTF_CACHELINE=128` is worth a sweep on both machines — <!-- MEASURE: rerun with
128 and compare -->. The pool uses `alignas(TF_CACHELINE)` (64 by default) on
`WorkerCounters` for exactly this reason.

### The deliberately-wrong ordering (Stage 9)

`chase_lev_deque_relaxed.hpp` is the correct deque with **one line changed**:
`push()` advances `bottom_` with a `relaxed` store instead of a `release` store.
That store is the only happens-before edge between the element write and a
thief's element read, so removing it makes the slot handoff a data race. The
slots are widened to a two-word cell so the effect is easy to see.

| how it's exercised | result |
|---|---|
| single-threaded / light tests | pass — casual testing does not catch it |
| `-O2` stress, x86-64 (TSO) | passes — x86 never reorders the two stores |
| `-O2` stress, **Apple M5** | **fails every run** — ~40k duplicate (stale) values per ~12M ops |
| ThreadSanitizer (Linux x86-64, CI) | **data race reported** on the ring cell |
| ThreadSanitizer (Apple M5) | detects it, but the arm64 sanitizer runtime deadlocks its own report path under concurrent races — see "what didn't work"; the Linux CI job is authoritative |

Run it: `make buggy-demo`. CI job `buggy-ordering-demo` fails if the variant ever
comes back clean. Full diff and the memory-model argument: [DESIGN.md](DESIGN.md#10-stage-9-the-deliberately-wrong-ordering).

### Scaling-limit analysis

`scripts/run_perf.sh scaling` captures one `perf stat` per thread count;
`scripts/analyze.py` joins those with the curve and emits
`results/perf/scaling_analysis.md` — a table of throughput / speedup /
efficiency / IPC / cache-miss-rate / context-switches / cpu-migrations per
thread count, and it points at the knee.

<!-- MEASURE: paste the analyze.py table, then one paragraph per candidate
cause with the number that rules it in or out:
  * falling IPC + rising cache-miss rate      -> memory bandwidth / coherence
  * flat IPC + rising ctx-switches/migrations -> scheduler overhead / oversubscription
  * flat IPC + flat cache + rising steal aborts (from --bench steal-rate) -> steal-path contention
  * none of the above moving                  -> genuine serial fraction (Amdahl)
On the M5 the knee at ~6 threads coincides with the 4 performance + 6 efficiency
core split; confirm on the homogeneous Linux box, where the knee should move. -->

The Apple M5 is heterogeneous (performance + efficiency cores), so its knee is
partly a core-type artefact. The Linux reference box is homogeneous and is the
one the analysis should conclude from.

---

## Comparison to production schedulers

Positioning, not a claim of parity. What each does that TaskForge doesn't, and
why it matters:

- **Intel TBB / oneTBB** — `task_arena` + `task_group`, range **partitioners**
  (`auto_partitioner` splits a range adaptively to the steal rate, so you don't
  hand-pick a grain), a `flow::graph` that is a strict superset of this DAG
  resolver (nodes with typed edges, buffering, back-pressure), and NUMA-aware
  arenas pinned to core sets. TaskForge has one global worker set, a fixed grain
  chosen by the caller, and a DAG that only carries control dependencies.
- **Go runtime scheduler** — G/M/P model: goroutines (G) multiplexed onto OS
  threads (M) via logical processors (P), each P with a 256-slot local run queue
  that other Ps steal from (the same idea as this deque) plus a global queue.
  Beyond scheduling: asynchronous **preemption** (a goroutine can be stopped
  mid-loop), a **netpoller** that parks goroutines blocked on I/O without
  burning an M, hand-off of the P when an M blocks in a syscall, and tight GC
  integration. TaskForge tasks run to completion, can't be preempted, and a task
  that blocks burns its worker.
- **Java `ForkJoinPool`** — Chase–Lev deques, but **continuation stealing**:
  `fork()` pushes the *continuation* and keeps running the child, so a stealer
  picks up the parent's remainder; this bounds stack depth better than the
  child-stealing + `help_until` here. Plus `ManagedBlocker` (tell the pool "I'm
  about to block, spin up a compensation thread"), `CountedCompleter` for
  push-style DAG completion, and a common pool sized to the machine. TaskForge
  does child stealing, has no managed-blocking escape hatch, and `help_until`
  keeps the waiting frame on the stack.

Common thread: the production schedulers all invest heavily in **what happens
when a task blocks or runs long** (preemption, compensation threads, I/O
parking). TaskForge deliberately doesn't — it targets CPU-bound run-to-completion
tasks, and says so.

---

## Parallel risk engine

A Monte Carlo Value-at-Risk / Expected Shortfall engine
(`include/taskforge/risk/`) built on top of everything above: same
`ThreadPool`/`MutexQueuePool`/`AsyncPool`, same DAG resolver, zero new
atomics. The point of building it is that it reuses, unmodified, exactly the
synchronization this project spent Phase 1 proving correct — the
`wait_idle()`/`pending_` accounting and the DAG's dependency counter are the
*entire* concurrency story for a workload that looks nothing like a
benchmark harness (option pricing, correlated random scenarios, risk
aggregation). The full happens-before argument is in
[DESIGN.md §13](DESIGN.md#13-phase-2-determinism-of-the-parallel-risk-engine).

### What it does

- **Numerical core** (`rng.hpp`, `black_scholes.hpp`): a deterministic
  SplitMix64 + Box-Muller normal generator seeded only from
  `(global_seed, chunk_index)` — never thread id or time — and closed-form
  Black-Scholes price/Greeks, including a hand-written inverse normal CDF
  (Acklam's rational approximation) for the delta-normal VaR below.
- **Portfolio** (`portfolio.hpp`): underlyings, a hand-written Cholesky
  decomposition (rejects non-positive-definite input rather than producing
  NaNs — `RiskPortfolio.CholeskyRejectsNonPositiveDefinite`/
  `CholeskyRejectsRankDeficientMatrix`), and a deterministic correlation-
  matrix generator that is positive definite by construction (a Gram matrix
  plus `eps*I`).
- **Serial, parallel, and DAG engines, one math** (`engine.hpp`):
  `simulate_chunk()` is the only place the GBM-plus-full-revaluation math
  lives; `run_serial`, `run_parallel<Pool>`, and `run_dag<Pool>` differ only
  in how a fixed, pre-planned set of (scenario range, seed) chunks gets
  executed. `run_parallel`/`run_dag` are templated on pool type, so
  identical code runs under all three pools.

### Correctness results (measured, this repo, dev box)

- **Full revaluation vs. delta-normal — the reason to build this at all.** A
  short at-the-money straddle (short 1,000 calls + 1,000 puts, 30-day
  expiry, 30% vol, 10-day horizon) is close to delta-neutral, so the linear
  approximation reports almost no risk for it. Full revaluation, which
  actually reprices the convexity, disagrees sharply:

  | | VaR₉₅ | ES₉₅ |
  |---|--:|--:|
  | delta-normal | 404.6 | 507.3 |
  | full revaluation | 3398.2 | 5131.8 |

  **8.4× understated VaR.** This is the textbook short-gamma failure mode of
  delta-only risk measures, reproduced from first principles.
- **Correlation genuinely drives the simulated shocks**, not just the
  closed-form formulas: two long, same-direction stock positions at ρ=0.9
  show 1.38× the VaR₉₅ of the same book at ρ=0.0 (2423 → 3341,
  full-revaluation, not delta-normal) — matching the closed-form prediction
  (`w₁²+w₂²+2ρw₁w₂` under the two correlations) almost exactly.
- **Firm Expected Shortfall is subadditive across desks — exactly, not
  approximately** (see DESIGN.md for why this holds by an argmax-over-subsets
  argument, not a simulation artifact). A 3-desk, 3-underlying book:

  | | VaR₉₅ | ES₉₅ |
  |---|--:|--:|
  | desk 0 | 1775 | 2197 |
  | desk 1 | 1939 | 2554 |
  | desk 2 | 550 | 635 |
  | **sum of desks** | **4265** | **5386** |
  | **firm** | **2660** | **3379** |

  Firm ES (3379) ≤ sum of desk ES (5386), as proven. Firm VaR (2660) is
  *also* less than the naive sum here (2660 vs 4265) — real diversification
  — but that is a property of this book's correlations, not a general
  guarantee: unlike ES, VaR has no averaging argument to fall back on, so
  there is no inequality that has to hold between firm VaR and summed desk
  VaRs in general (`RiskEngineDag.FirmVarIsNotEqualToSumOfDeskVars`,
  DESIGN.md §13 and `engine.hpp`'s `run_dag` comment).
- **Bit-identical results across 1/2/4/8 threads and all three pools**, for
  both the chunked parallel engine and the DAG engine
  (`RiskEngineParallel.DeterministicAcrossThreadCountsAndPools`,
  `RiskEngineDag.DagResultDeterministicAcrossThreadCounts`) — asserted as
  exact equality, not "close enough".

### Benchmark results

<!-- MEASURE: paste results/charts/risk.png and the reference-machine numbers -->
On the dev box (Apple M5, 10 cores, 200k scenarios, 4-underlying/3-position
book) the work-stealing and mutex pools scale almost identically — 5.1× and
5.6× at 10 threads — while `std::async` stays flat at ~1×. The chunk-size
sweep peaks around 250–5,000 scenarios/chunk (~6×10⁷ paths/sec) and collapses
at both ends: below ~50 scenarios/chunk, per-chunk scheduling overhead
dominates; a single chunk covering all 200k scenarios gives every pool
*identical* throughput regardless of thread count, since there is then
nothing left to parallelize. Full numbers:
`results/csv/sample_risk.csv` (run with
`taskforge_bench --bench risk --n-scenarios 200000`).

> **Work-stealing ≈ mutex here for the same reason as the Phase 1 scaling
> sweep (see "what didn't work" below): `run_parallel` submits every chunk
> from the calling thread, not from inside a worker, so every chunk goes
> through the injection queue regardless of pool.** The Chase-Lev deque's
> steal path simply isn't exercised by this workload shape — reusing it
> faithfully reproduced the same finding in a completely different domain,
> which is itself informative: injection-bound workloads are injection-bound
> no matter what they compute.

The pricing-book benchmark (`--bench risk-pricing-book`) deliberately mixes
85% cheap closed-form prices, 10% moderate (price + Greeks), and 5%
expensive path-dependent Asian-option Monte Carlo prices (no closed form, so
they're genuinely slow) — 5,000 tasks, submitted through one generator task
so the steal path *is* exercised this time. Dev box, 10 threads:

| pool | wall time (median) | tasks executed | successful steals |
|---|--:|--:|--:|
| work-stealing | 45.35 ms | 5,000 | 4,572 |
| mutex | 45.07 ms | 5,000 | — (no steal mechanism) |

Despite thousands of real steals correcting real load imbalance, wall time is
a statistical tie. This connects directly to the Phase 1 granularity-sweep
finding: work-stealing's advantage lives at the fine-grained end, and even
the "cheap" tasks here (one Black-Scholes evaluation) are large enough,
relative to a mutex lock/unlock, that queue contention was never the
bottleneck — the same crossover the granularity sweep found, reached from
the opposite direction.
`results/csv/sample_risk_pricing_book.csv` has the full per-worker data.

---

## What didn't work

Kept as we went; failed experiments are the most credible part of this.

- **`MutexQueuePool::shutdown()` had a textbook lost-wakeup race.** It set
  `done_` and called `q_cv_.notify_all()` without ever touching `q_mtx_`. A
  worker inside `q_cv_.wait(lk, pred)` re-checks its predicate only while
  holding `q_mtx_`, and only transitions into the OS-level wait atomically
  with releasing it — so a worker caught between "predicate read `false`" and
  "finished registering as a waiter" could see neither the new `done_` nor the
  notify. `q_cv_.wait` has no timeout (unlike `ThreadPool`'s sleep, which has a
  500µs backstop for exactly this reason), so a missed wakeup there hangs
  forever. Caught by Linux CI (`ci-tsan`, both gcc and clang) as a 600s test
  timeout on `PoolContract.SubmitAfterShutdownThrows<MutexQueuePool>` —
  `macos-arm` CI and every local Apple Silicon run passed, so this was a real,
  rare, timing-dependent race, not flakiness: a tight construct/shutdown loop
  reproduced it locally after ~18k iterations under TSan. Fix: take and
  release `q_mtx_` once, after `done_` is set, before `notify_all()` — the
  same fence `ThreadPool::shutdown()` already used. 150k iterations clean
  after the fix.
- **Standalone `std::atomic_thread_fence` (the textbook LPCN formulation).** The
  deque first followed LPCN literally: a `release` fence in `push()` and
  `seq_cst` fences separating the owner's and thief's last-element accesses.
  It was correct on hardware and TSan-clean *in isolation* (single owner, single
  thief). Under the pool's workload — every thread rapidly alternating owner
  ops on its own deque and thief ops on others — **ThreadSanitizer on arm64
  deadlocked inside its own runtime**. TSan's modelling of standalone fences is
  a documented weak spot. Switched to carrying the orderings on the operations
  themselves: a `release` store on `bottom_` in `push()`, and `seq_cst` on the
  `bottom_`/`top_` accesses that form the Dekker pair. Provably equivalent (the
  argument is a single total order over the seq_cst ops), it is what
  crossbeam-deque / Taskflow-style implementations do in practice, and it is
  TSan-clean. Details in [DESIGN.md](DESIGN.md).
- **ThreadSanitizer's own report path deadlocks on arm64 (Apple M5) under a
  multi-threaded data race.** Seen twice: the Stage 9 buggy deque under TSan, and
  (earlier, before it was fixed) an ASan `BUS` pile-up in Stage 3. With several
  threads hitting the same race/fault at once, the sanitizer runtime appears to
  wedge in `Die()` / the reporting lock and produces no output. Worked around by
  keeping sanitized stress harnesses to 1–2 racing threads locally and treating
  the **Linux x86-64 CI** sanitizer jobs as authoritative. Not a bug in
  TaskForge, but it shaped how the tests are written.
- **The flat-workload benchmark didn't test what it looked like it tested.** The
  first scaling/granularity sweeps submit N independent tasks from the main
  thread. Building the Stage 7 steal-rate instrumentation made it obvious that
  every one of those tasks goes through the injection queue (single-owner rule),
  so `steals_success` was ~0 and the work-stealing pool was, for that workload,
  a mutex queue with extra failed steal attempts. The deque only earns its keep
  when work originates *on* worker deques — recursive fork–join and imbalance.
  The steal-rate and DAG sweeps were rebuilt around that; the flat sweeps stayed
  as an honest "here's where it doesn't help" data point.
- **`std::function` per task is a measurable floor.** On sub-microsecond tasks
  the `new Task{std::function}` allocation is a real fraction of per-task cost,
  and it's paid by every pool (the mutex baseline `new`s a `Task` too), which is
  part of why work-stealing ≈ mutex-queue at the fine end for flat workloads. A
  small-buffer callable would lift that floor; it's applied uniformly across
  pools so the *comparison* stays fair, and left as future work.
- <!-- MEASURE: fill from the reference-machine runs, e.g. spin/yield/sleep
  backoff constants — the current 128/64/500µs were picked, not measured -->
- <!-- MEASURE: TF_CACHELINE 64 vs 128 on Apple Silicon (Stage 8) -->
- **(Phase 2) A second `.cpp` including `thread_pool.hpp` doesn't link on
  AppleClang/ld64.** `risk_bench.cpp` was first written as a normal
  translation unit, like `workload.cpp`. It compiled but failed to link:
  `duplicate symbol 'thread-local wrapper routine for
  taskforge::ThreadPool::t_pool_'`. `t_pool_`/`t_worker_` are declared
  `inline thread_local` at namespace scope — valid C++17 (inline variables
  are explicitly meant to have the same definition appear in multiple TUs),
  but this trips a real ld64 limitation with the lazy-initialization
  "wrapper routine" the compiler generates for `thread_local`, which was
  simply never exercised before since only `bench_main.cpp` ever included
  `thread_pool.hpp`. Not a bug in TaskForge's code; worked around (not
  "fixed", since there's nothing in this repo to fix) by making
  `risk_bench.hpp` header-only, like the rest of `include/taskforge/` —
  which is a good reminder of *why* that library is header-only to begin
  with.
- **(Phase 2) The pricing-book benchmark's first draft didn't exercise
  stealing either — same root cause as the Phase 1 entry above, rediscovered.**
  The first version submitted every task in the book with a flat
  `for (...) pool.submit(...)` loop from the benchmark's own thread.
  `steals_success` came back ~0 for the work-stealing pool too: external
  submits always go through the injection queue (single-writer-per-deque
  rule), so a flat loop can't show a steal-vs-mutex comparison no matter how
  uneven the task costs are. Fixed by submitting the whole book through one
  generator task instead (`submit_book`), landing every leaf task on that
  one worker's own deque first — `bench_steal_rate`'s existing convention,
  applied to a new workload, catching the exact same mistake a second time
  confirms it's a real, easy-to-make error and not a one-off.
- **(Phase 2) A fixed-fraction-of-spot step size broke
  `GammaMatchesSecondDifference` on the 1-day-horizon sample input.** Gamma's
  central second-difference has worse numerical conditioning than a first
  difference (near-equal-value subtraction, divided by `h²`), so `h` needs to
  scale with the option's own curvature scale — roughly `S·σ·√T`, the "one
  standard deviation of spot move" width — not spot alone. `h = 1e-2·spot`
  is fine when `T=1` but is comparable to (not tiny next to) `S·σ·√T ≈ $1`
  when `T = 1/365`, so truncation error dominated and the test's error
  looked like a formula bug. Diagnosed by sweeping `h` relative to
  `S·σ·√T` across every sample input (`~1e-5..1e-6` relative error at
  `h = 1e-2·scale`, consistently); fixed by scaling `h` to that quantity
  instead, with the empirical justification left in the test as a comment.

---

## License

MIT (see `LICENSE`).
