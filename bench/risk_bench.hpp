// risk_bench.hpp -- Phase 2 / P2-5: the parallel risk engine's own
// benchmarks, dispatched from bench_main.cpp as `--bench risk` and
// `--bench risk-pricing-book`.
//
// Header-only (every function `inline`), unlike workload.hpp/.cpp's split:
// thread_pool.hpp defines its per-thread `t_pool_`/`t_worker_` state as
// `inline thread_local` at namespace scope, which is valid C++17 (inline
// variables are explicitly meant to be definable identically in multiple
// translation units) but trips a real AppleClang/ld64 linker limitation --
// "duplicate symbol 'thread-local wrapper routine for ...'" -- the moment
// TWO .cpp files both include thread_pool.hpp into the same binary. Keeping
// this header-only, so only bench_main.cpp's single translation unit ever
// instantiates ThreadPool-using code, sidesteps it entirely; this is also
// why thread_pool.hpp and the rest of include/taskforge/ are themselves
// header-only.
//
// Deliberately its own schema(s), not bench_common.hpp's CsvWriter: that
// schema is shaped around "N tasks of ~task_us each", which does not fit a
// Monte Carlo VaR run (whose natural units are scenarios/paths and chunks,
// not individually-sized tasks). bench_common.hpp's Clock, Summary, and
// thread-pinning helpers are reused as-is.
//
// bench_risk_timing covers (a) the thread-count scaling sweep (ThreadPool /
// MutexQueuePool / AsyncPool, speedup, efficiency, paths/sec) and (b) the
// chunk-size sweep, under one shared schema distinguished by a `variant`
// column, plus (d) the Amdahl's-law serial/parallel split: `serial_ms` (the
// time to sort-and-reduce the losses array into a VaR/ES -- single-threaded
// regardless of how many workers ran the simulation) is measured once per
// pool and reported alongside every scaling row, so the fraction of total
// time that can never be parallelized away is visible next to the sweep it
// bounds.
//
// bench_risk_pricing_book covers (c): an uneven-cost "pricing book" workload
// (mostly cheap vanilla pricings, a few deliberately expensive ones),
// work-stealing vs mutex, with per-worker steal counts from pool.stats() --
// its own, per-worker-row schema, matching bench_main.cpp's existing
// bench_steal_rate's shape for the same reason (an aggregate wall-time
// schema cannot also carry one row per worker). Dispatched as its own
// --bench value rather than folded into "risk" for the same row-shape
// reason steal-rate and false-sharing are already separate --bench values
// in bench_main.cpp, not sub-variants of "granularity"/"scaling".

#pragma once

#include "bench_common.hpp"

#include "taskforge/async_pool.hpp"
#include "taskforge/mutex_queue_pool.hpp"
#include "taskforge/risk/black_scholes.hpp"
#include "taskforge/risk/engine.hpp"
#include "taskforge/risk/portfolio.hpp"
#include "taskforge/risk/rng.hpp"
#include "taskforge/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace taskforge::bench {

struct RiskBenchArgs {
  std::string pool = "all";    // "all" or comma list of workstealing|mutex|async
  int reps = 7;
  int warmup = 2;
  std::int64_t n_scenarios = 0;  // 0 => bench-specific default
  std::int64_t chunk_size = 0;   // 0 => bench-specific default (chunksize sweep ignores this)
  std::int64_t n_tasks = 0;      // 0 => bench-specific default (pricing-book only)
  bool pin = false;
};

namespace risk_bench_detail {

inline unsigned hw() {
  unsigned h = std::thread::hardware_concurrency();
  return h ? h : 4;
}

// Same small selector bench_main.cpp uses for --pool; duplicated rather than
// shared across translation units for four lines of code.
inline bool pool_selected(const std::string& sel, const char* name) {
  if (sel == "all") return true;
  std::string tok;
  for (std::size_t i = 0; i <= sel.size(); ++i) {
    if (i == sel.size() || sel[i] == ',') {
      if (tok == name) return true;
      tok.clear();
    } else {
      tok += sel[i];
    }
  }
  return false;
}

// A representative-sized book: 4 underlyings, a mix of stock and option
// positions, single desk (run_parallel/compute_var_es don't care about desk
// structure -- that's P2-4's DAG path).
inline taskforge::risk::Portfolio make_bench_portfolio() {
  using namespace taskforge::risk;
  const std::vector<Underlying> underlyings = {
      {"EQ1", 100.0, 0.22},
      {"EQ2", 55.0, 0.35},
      {"EQ3", 220.0, 0.18},
      {"EQ4", 40.0, 0.50},
  };
  Portfolio p = make_portfolio(underlyings, 0.025, 777001);
  p.stocks = {{0, 500.0, 0}, {2, -200.0, 0}};
  p.options = {
      {0, OptionType::kCall, 105.0, 0.50, 300.0, 0},
      {1, OptionType::kPut, 50.0, 0.25, -400.0, 0},
      {3, OptionType::kCall, 45.0, 0.10, 600.0, 0},
  };
  return p;
}

inline taskforge::risk::ScenarioParams make_base_params(const RiskBenchArgs& a) {
  taskforge::risk::ScenarioParams params;
  params.horizon_years = 10.0 / 365.0;
  params.drift = 0.0;
  params.n_scenarios = a.n_scenarios > 0 ? a.n_scenarios : 300000;
  params.chunk_size = a.chunk_size > 0 ? a.chunk_size : 1000;
  params.seed = 55555;
  return params;
}

template <class Pool>
void pin_pool(Pool& pool, unsigned threads, bool pin) {
  if (!pin || !pinning_available()) return;
  std::atomic<int> done{0};
  for (unsigned c = 0; c < threads; ++c) {
    pool.submit([c, &done] {
      pin_this_thread(c);
      done.fetch_add(1);
    });
  }
  pool.wait_idle();
}

// ---------------------------------------------------------------------------
// (a) Thread-count scaling sweep, plus the serial (sort + reduce) time that
// bounds it (Amdahl's-law figure, item (d)).
// ---------------------------------------------------------------------------
template <class Pool>
void scaling_for_pool(const RiskBenchArgs& a, const char* pool_name, std::FILE* out,
                     const taskforge::risk::Portfolio& portfolio,
                     const taskforge::risk::ScenarioParams& base_params, unsigned hw_n) {
  using namespace taskforge::risk;
  double baseline_ms = 0.0;
  double serial_ms = -1.0;  // measured once, independent of thread count

  for (unsigned t = 1; t <= hw_n; ++t) {
    std::vector<double> kept;
    kept.reserve(static_cast<std::size_t>(a.reps));
    std::vector<double> last_losses;
    const int total = a.reps + a.warmup;
    for (int r = 0; r < total; ++r) {
      Pool pool(t);
      pin_pool(pool, t, a.pin);
      const auto t0 = Clock::now();
      std::vector<double> losses = run_parallel(pool, portfolio, base_params);
      const double ms = ms_between(t0, Clock::now());
      if (r >= a.warmup) kept.push_back(ms);
      if (r == total - 1) last_losses = std::move(losses);
    }

    if (serial_ms < 0.0) {
      const auto t0 = Clock::now();
      const QuantileResult qr = compute_var_es(last_losses, 0.95);
      serial_ms = ms_between(t0, Clock::now());
      (void)qr;
    }

    const Summary s = Summary::of(kept);
    if (t == 1) baseline_ms = s.median_ms;
    const double speedup = (baseline_ms > 0.0 && s.median_ms > 0.0) ? baseline_ms / s.median_ms : 0.0;
    const double efficiency = speedup / static_cast<double>(t);
    const double paths_per_sec =
        s.median_ms > 0.0 ? static_cast<double>(base_params.n_scenarios) / (s.median_ms / 1000.0)
                          : 0.0;
    const std::int64_t n_chunks =
        (base_params.n_scenarios + base_params.chunk_size - 1) / base_params.chunk_size;

    std::fprintf(out,
                 "risk,scaling,%s,%u,%lld,%lld,%lld,%zu,"
                 "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,"
                 "%.2f,%.4f,%.4f,%.5f,%d\n",
                 pool_name, t, static_cast<long long>(base_params.n_scenarios),
                 static_cast<long long>(base_params.chunk_size), static_cast<long long>(n_chunks),
                 s.reps, s.median_ms, s.mean_ms, s.p95_ms, s.p99_ms, s.stddev_ms, s.min_ms,
                 s.max_ms, paths_per_sec, speedup, efficiency, serial_ms,
                 a.pin && pinning_available() ? 1 : 0);
    std::fflush(out);
  }
}

// ---------------------------------------------------------------------------
// (b) Chunk-size sweep at a fixed thread count: too-small chunks pay
// scheduling/submission overhead per chunk; a single chunk covering all
// n_scenarios has zero parallelism no matter how many threads are available.
// Somewhere between is the sweet spot this sweep is meant to expose.
// ---------------------------------------------------------------------------
template <class Pool>
void chunksize_for_pool(const RiskBenchArgs& a, const char* pool_name, std::FILE* out,
                       const taskforge::risk::Portfolio& portfolio,
                       const taskforge::risk::ScenarioParams& base_params, unsigned threads) {
  using namespace taskforge::risk;
  const std::vector<std::int64_t> sizes = {10,   25,   50,   100,  250,  500,
                                           1000, 2500, 5000, 10000, base_params.n_scenarios};
  for (std::int64_t cs : sizes) {
    ScenarioParams params = base_params;
    params.chunk_size = cs;

    std::vector<double> kept;
    kept.reserve(static_cast<std::size_t>(a.reps));
    for (int r = 0; r < a.reps + a.warmup; ++r) {
      Pool pool(threads);
      pin_pool(pool, threads, a.pin);
      const auto t0 = Clock::now();
      std::vector<double> losses = run_parallel(pool, portfolio, params);
      const double ms = ms_between(t0, Clock::now());
      if (r >= a.warmup) kept.push_back(ms);
    }

    const Summary s = Summary::of(kept);
    const double paths_per_sec =
        s.median_ms > 0.0 ? static_cast<double>(params.n_scenarios) / (s.median_ms / 1000.0) : 0.0;
    const std::int64_t n_chunks = (params.n_scenarios + cs - 1) / cs;

    std::fprintf(out,
                 "risk,chunksize,%s,%u,%lld,%lld,%lld,%zu,"
                 "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,"
                 "%.2f,0,0,0,%d\n",
                 pool_name, threads, static_cast<long long>(params.n_scenarios),
                 static_cast<long long>(cs), static_cast<long long>(n_chunks), s.reps,
                 s.median_ms, s.mean_ms, s.p95_ms, s.p99_ms, s.stddev_ms, s.min_ms, s.max_ms,
                 paths_per_sec, a.pin && pinning_available() ? 1 : 0);
    std::fflush(out);
  }
}

// ---------------------------------------------------------------------------
// (c) The pricing-book workload: mostly cheap vanilla pricings, a few
// deliberately expensive ones (a path-dependent arithmetic-average Asian
// call, priced by its own small Monte Carlo since it has no closed form --
// this is the ONLY place in the whole risk engine Asian options appear; they
// are not part of the VaR/ES revaluation path, only this benchmark's
// deliberately uneven workload).
// ---------------------------------------------------------------------------
enum class TaskKind { kCheap, kModerate, kExpensive };

struct PricingTask {
  TaskKind kind;
  double spot, strike, rate, vol, t;
  std::uint64_t seed;
};

inline double price_asian_call_mc(double spot, double strike, double rate, double vol,
                                  double expiry, int n_steps, int n_paths, std::uint64_t seed) {
  using taskforge::risk::RngStream;
  RngStream rng(seed);
  const double dt = expiry / static_cast<double>(n_steps);
  const double drift = (rate - 0.5 * vol * vol) * dt;
  const double diffusion = vol * std::sqrt(dt);

  double sum_payoff = 0.0;
  for (int p = 0; p < n_paths; ++p) {
    double s = spot;
    double avg = 0.0;
    for (int step = 0; step < n_steps; ++step) {
      s *= std::exp(drift + diffusion * rng.next_standard_normal());
      avg += s;
    }
    avg /= static_cast<double>(n_steps);
    sum_payoff += std::max(avg - strike, 0.0);
  }
  return std::exp(-rate * expiry) * sum_payoff / static_cast<double>(n_paths);
}

inline double price_one(const PricingTask& task) {
  using namespace taskforge::risk;
  const BlackScholesInputs in{task.spot, task.strike, task.rate, task.vol, task.t};
  switch (task.kind) {
    case TaskKind::kCheap:
      return black_scholes_price(OptionType::kCall, in);
    case TaskKind::kModerate: {
      const double price = black_scholes_price(OptionType::kCall, in);
      const OptionGreeks g = black_scholes_greeks(OptionType::kCall, in);
      return price + g.delta + g.gamma + g.vega + g.theta + g.rho;
    }
    case TaskKind::kExpensive:
      // ~50 steps x 2000 paths = 100,000 RNG draws, several orders of
      // magnitude more expensive than the closed-form cases above -- the
      // deliberately heavy tail of the book's cost distribution.
      return price_asian_call_mc(task.spot, task.strike, task.rate, task.vol, task.t, 50, 2000,
                                 task.seed);
  }
  return 0.0;
}

// 85% cheap / 10% moderate / 5% expensive: most of a real pricing book is
// vanilla; a handful of exotics make the cost distribution heavy-tailed, the
// same shape Phase 1's "imbalanced" workload explored in the abstract.
inline std::vector<PricingTask> make_pricing_book(std::int64_t n_tasks, std::uint64_t seed) {
  using namespace taskforge::risk;
  std::vector<PricingTask> book;
  book.reserve(static_cast<std::size_t>(n_tasks));
  RngStream rng(seed);
  for (std::int64_t i = 0; i < n_tasks; ++i) {
    const double u = rng.next_uniform();
    const TaskKind kind =
        u < 0.85 ? TaskKind::kCheap : (u < 0.95 ? TaskKind::kModerate : TaskKind::kExpensive);
    const double spot = 50.0 + rng.next_uniform() * 150.0;
    const double strike = spot * (0.8 + rng.next_uniform() * 0.4);
    const double vol = 0.15 + rng.next_uniform() * 0.35;
    const double expiry = 0.1 + rng.next_uniform() * 1.9;
    book.push_back(PricingTask{kind, spot, strike, 0.02, vol, expiry,
                               chunk_seed(seed, static_cast<std::uint64_t>(i) + 1)});
  }
  return book;
}

// Submits the whole book through ONE generator task rather than looping
// pool.submit() from the benchmark's own (non-worker) thread. That
// distinction matters: submit() called from outside any worker always goes
// through the pool's shared injection queue (see thread_pool.hpp's
// enqueue()), so a flat "submit every task from main" loop would never
// exercise the steal path at all -- every worker would just pull from the
// same injection queue, indistinguishable from the mutex baseline's own
// shared-queue contention. Submitting from INSIDE a worker task instead
// lands every book task on that one worker's own deque first, so the other
// idle workers must steal to pick up their share -- exactly
// bench_steal_rate's existing convention in bench_main.cpp, reused here for
// the same reason.
template <class Pool>
void submit_book(Pool& pool, const std::vector<PricingTask>& book) {
  pool.submit([&pool, &book] {
    for (const PricingTask& task : book) {
      pool.submit([&task] {
        volatile double sink = price_one(task);
        (void)sink;
      });
    }
  });
}

template <class Pool>
void pricing_book_for_pool(const RiskBenchArgs& a, const char* pool_name, std::FILE* out,
                          const std::vector<PricingTask>& book, unsigned threads) {
  std::vector<double> kept;
  kept.reserve(static_cast<std::size_t>(a.reps));
  for (int r = 0; r < a.reps + a.warmup; ++r) {
    Pool pool(threads);
    const auto t0 = Clock::now();
    submit_book(pool, book);
    pool.wait_idle();
    const double ms = ms_between(t0, Clock::now());
    if (r >= a.warmup) kept.push_back(ms);
  }
  const Summary s = Summary::of(kept);

  // One extra, untimed run purely to read per-worker steal counts -- same
  // convention bench_main.cpp's bench_steal_rate uses: instrumentation is a
  // single-shot look at the mechanism, not something averaged across reps.
  Pool pool(threads);
  submit_book(pool, book);
  pool.wait_idle();
  const taskforge::PoolStats st = pool.stats();

  for (std::size_t w = 0; w < st.per_worker.size(); ++w) {
    const taskforge::WorkerCounters& c = st.per_worker[w];
    std::fprintf(out,
                 "risk,pricing-book,%s,%u,%zu,%zu,%.5f,%.5f,%u,"
                 "%llu,%llu,%llu,%llu,%llu,%llu\n",
                 pool_name, threads, book.size(), s.reps, s.median_ms, s.p95_ms,
                 static_cast<unsigned>(w), static_cast<unsigned long long>(c.tasks_executed),
                 static_cast<unsigned long long>(c.own_pops),
                 static_cast<unsigned long long>(c.steals_success),
                 static_cast<unsigned long long>(c.steals_abort),
                 static_cast<unsigned long long>(c.steals_empty),
                 static_cast<unsigned long long>(c.inject_takes));
  }
  std::fflush(out);
}

}  // namespace risk_bench_detail

inline void bench_risk_timing(const RiskBenchArgs& a, std::FILE* out, bool header) {
  using namespace risk_bench_detail;
  if (header) {
    std::fprintf(out,
                 "bench,variant,pool,threads,n_scenarios,chunk_size,n_chunks,reps,"
                 "median_ms,mean_ms,p95_ms,p99_ms,stddev_ms,min_ms,max_ms,"
                 "paths_per_sec,speedup,efficiency,serial_ms,pinned\n");
  }

  const taskforge::risk::Portfolio portfolio = make_bench_portfolio();
  const taskforge::risk::ScenarioParams params = make_base_params(a);
  const unsigned hw_n = hw();

  struct WS { using type = taskforge::ThreadPool; };
  struct MX { using type = taskforge::MutexQueuePool; };
  struct AS { using type = taskforge::AsyncPool; };

  auto scaling_go = [&](const char* name, auto tag) {
    using P = typename decltype(tag)::type;
    scaling_for_pool<P>(a, name, out, portfolio, params, hw_n);
  };
  if (pool_selected(a.pool, "workstealing")) scaling_go("workstealing", WS{});
  if (pool_selected(a.pool, "mutex")) scaling_go("mutex", MX{});
  if (pool_selected(a.pool, "async")) scaling_go("async", AS{});

  auto chunksize_go = [&](const char* name, auto tag) {
    using P = typename decltype(tag)::type;
    chunksize_for_pool<P>(a, name, out, portfolio, params, hw_n);
  };
  if (pool_selected(a.pool, "workstealing")) chunksize_go("workstealing", WS{});
  if (pool_selected(a.pool, "mutex")) chunksize_go("mutex", MX{});
  if (pool_selected(a.pool, "async")) chunksize_go("async", AS{});
}

inline void bench_risk_pricing_book(const RiskBenchArgs& a, std::FILE* out, bool header) {
  using namespace risk_bench_detail;
  if (header) {
    std::fprintf(out,
                 "bench,variant,pool,threads,n_tasks,reps,wall_median_ms,wall_p95_ms,"
                 "worker,tasks_executed,own_pops,steals_success,steals_abort,steals_empty,"
                 "inject_takes\n");
  }

  const std::int64_t n_tasks = a.n_tasks > 0 ? a.n_tasks : 20000;
  const std::vector<PricingTask> book = make_pricing_book(n_tasks, 909090);
  const unsigned threads = hw();

  // Only work-stealing vs mutex, per the spec: AsyncPool spawns one OS
  // thread per task (see bench_main.cpp's bench_dag comment for the same
  // concern at larger N), and the point of this benchmark is specifically
  // the work-stealing-vs-mutex contrast under an uneven cost distribution.
  struct WS { using type = taskforge::ThreadPool; };
  struct MX { using type = taskforge::MutexQueuePool; };
  if (pool_selected(a.pool, "workstealing")) {
    pricing_book_for_pool<WS::type>(a, "workstealing", out, book, threads);
  }
  if (pool_selected(a.pool, "mutex")) {
    pricing_book_for_pool<MX::type>(a, "mutex", out, book, threads);
  }
}

}  // namespace taskforge::bench
