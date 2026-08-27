# TaskForge — Lock-Free Work-Stealing Task Scheduler

## What this is
A high-throughput task scheduler in modern C++: per-worker deques with lock-free
work stealing, plus a DAG dependency resolver. The benchmark and analysis proving
where it wins — and where it does not — is as much the deliverable as the scheduler.

Portfolio project for NVIDIA internship applications (Systems Software, Software
Engineering). Correctness under concurrency and honest measurement matter as much
as raw speed. The bar is: an interviewer reads the repo and concludes the author
understands memory models, not that the author found a paper and transcribed it.

## Who I am
MS CS student. Strong Python/Java/Go, backend and distributed systems. I have
written multithreaded Python (TCP/UDP servers with mutex synchronization) and I
grade concurrent Java systems as a TA — threading concepts are familiar.

**I have not written production C++, and I have never written lock-free code.**
This project exists to change that. Explain every memory-ordering decision as you
go. If I can't explain why a line uses acquire instead of seq_cst, that line is a
liability, not an asset.

## Environment
- macOS for development, Linux (USC CARC or VM) for perf profiling.
- No exotic hardware assumed. Must build on modern x86-64 Linux and Apple Silicon macOS.
- C++17. Clang and GCC both compile clean under -Wall -Wextra -Wpedantic.
- Note where x86 TSO vs ARM weak ordering could hide bugs. Test on both if possible —
  a relaxed-ordering bug that x86 papers over will surface on Apple Silicon. If we
  find one, that goes in the README; it's a genuinely strong finding.

## Locked design decisions
Do not revisit without asking me.

- **Chase-Lev work-stealing deque**, implemented from the paper. Owner pushes/pops
  the bottom; thieves steal from the top.
- **Lock-free via std::atomic with explicit memory ordering.** No mutexes in the hot
  path. Every non-relaxed ordering gets a comment naming what it synchronizes with.
- **Baselines are part of the project**, not an afterthought: mutex-protected global
  queue, and std::async.
- **CMake + GoogleTest.** Structured like a real project, not a single-file demo.

## Architecture

  Submit -> [ DAG resolver ] -> ready tasks -> per-worker deques
                                                   |  |  |
                                              worker threads
                                            (pop own / steal others)

- One deque per worker. Owner at one end, thieves at the other — this is what makes
  the common case contention-free, and the README must explain why.
- Random victim selection on steal; document why round-robin is worse.
- Idle workers back off before spinning hot. Document the backoff strategy and what
  you measured to choose it.
- DAG resolver: tasks declare prerequisites, become schedulable when the dependency
  count hits zero. Cycles detected and rejected at submission.

## Correctness — non-negotiable
- **ThreadSanitizer and AddressSanitizer clean**, both as CMake configurations and
  both in CI. A TSan finding is a bug, not a flake.
- Deque tested in isolation before the scheduler: single-threaded push/pop ordering
  first, then concurrent steal correctness — no duplicates, no losses, no phantom items.
- Sustained high-contention stress tests, not a handful of iterations. Concurrency
  bugs hide in short runs.
- **ABA problem:** state explicitly in DESIGN.md how the implementation avoids it.
- **Exception safety:** what happens when a task throws? Define the policy and test it.
  A scheduler that deadlocks on an exception is broken.
- **Clean shutdown:** joining with tasks in flight, with tasks queued, and with
  workers mid-steal. All three tested.

## Benchmarking rules
- Measure against **both** baselines on identical workloads.
- Sweep thread count 1..hardware_concurrency. Report the scaling curve, and identify
  where it stops being linear and why.
- **Sweep task granularity — this is the key experiment.** Work stealing wins big on
  many small tasks and converges to no advantage as tasks lengthen. Find and report
  the crossover point. Do not report only the flattering fine-grained number; the
  honest curve is more impressive than a single ratio.
- Report median, p95, p99, and variance across repetitions. Discard warmup.
- std::chrono::steady_clock. Pin threads where it reduces noise and say so.
- Workloads: fine-grained (microsecond), coarse (millisecond), mixed, imbalanced
  (deliberately uneven task durations — this is where stealing should shine most),
  and a DAG workload (recursive fork-join, e.g. parallel merge sort or Fibonacci).

## What elevates this beyond a tutorial implementation
Build these — they are the difference between "implemented a known algorithm" and
"understands concurrent systems":

- **Steal-rate instrumentation.** Count successful steals, failed steals, and
  spin-wait time per worker. Plot steal rate against load imbalance. This shows the
  mechanism working, not just the outcome.
- **The false-sharing experiment, run deliberately in two steps.** First implement
  per-worker state WITHOUT cache-line alignment and measure it. Then add alignas(64)
  and measure again. Report both with perf cache-miss counts. Doing this on purpose
  and documenting it is a deliverable, not a mistake to hide.
- **A deliberately wrong version.** Implement one deque operation with relaxed
  ordering where acquire/release is required. Show that it passes casual testing,
  then show TSan catching it (or a stress test failing on ARM). Document it. This
  single section demonstrates more understanding than a correct implementation alone.
- **Scaling limit analysis.** When throughput stops scaling with threads, explain
  why using perf data — memory bandwidth, cache coherence traffic, scheduler
  overhead, or contention on the steal path. Don't just show the curve; explain it.
- **Comparison to production schedulers.** A short section in the README positioning
  this against Intel TBB, Go's runtime scheduler, and Java's ForkJoinPool. What do
  they do that this doesn't, and why. Shows awareness beyond your own code.

## Profiling
- perf scripts capturing cache misses, cache references, context switches, CPU
  migrations, instructions per cycle.
- Correlate profile data with the scaling curve so every performance claim in the
  README has evidence behind it.

## Deliverables
- CMake with Debug, Release, ASan, TSan configurations.
- GoogleTest suite: deque unit tests, concurrency tests, scheduler integration,
  DAG tests, exception-safety tests, shutdown tests, stress tests.
- Benchmark harness producing CSV; Python scripts rendering scaling, granularity,
  and steal-rate charts.
- GitHub Actions CI running the suite under both sanitizers.
- **DESIGN.md — the most important file in the repo.** For every atomic operation:
  which ordering, what it synchronizes with, why weaker breaks and why stronger
  costs. Written so I can study from it and answer interview questions from it.
- README: architecture, benchmark results with clearly marked placeholders for my
  measured numbers, the scaling-limit analysis, the production-scheduler comparison,
  and a "what didn't work" section maintained as we go.

## Build order — check in with me between stages
1. CMake skeleton, GoogleTest, sanitizer configs, CI
2. Chase-Lev deque standalone + unit and concurrency tests
3. Thread pool: work stealing, backoff, clean shutdown, exception policy
4. Mutex-queue and std::async baselines
5. Benchmark harness: granularity sweep, scaling sweep, imbalanced workloads, charts
6. DAG resolver + cycle detection
7. Steal-rate instrumentation and its charts
8. False-sharing experiment (both versions, measured)
9. The deliberately-wrong-ordering demonstration
10. perf profiling, scaling-limit analysis, DESIGN.md, README

## Working style
- Explain the approach in plain terms BEFORE writing each stage. For anything atomic,
  explain the memory ordering first and wait for me to confirm I follow it. Do not
  proceed past stage 2 until I can restate the acquire/release reasoning in my own words.
- Small commits per stage. Never one large initial commit.
- Clarity over cleverness. An interviewer is going to read this code.
- Maintain the README's "what didn't work" section continuously — failed optimizations
  and the reasoning behind them are the most credible part of the project.