// bench_main.cpp -- the benchmark driver.
//
//   taskforge_bench --bench <granularity|scaling|imbalanced|dag|steal-rate> [options]
//
// Common options:
//   --pool <workstealing|mutex|async|all>   (default: all)
//   --threads <N|sweep>                     (default: hardware_concurrency)
//   --reps <N>            repetitions kept              (default 11)
//   --warmup <N>          repetitions discarded first   (default 3)
//   --task-us <F>         per-task work, microseconds   (per-bench default)
//   --tasks <N>           task count (fixed workloads)  (default: auto)
//   --imbalance <F>       p99/median for --bench imbalanced (default 32)
//   --pin                 pin worker threads (Linux; recorded in CSV)
//   --out <file>          append CSV here (default: stdout)
//   --no-header           do not print the CSV header row
//
// The CSV schema is in bench_common.hpp. Every sweep appends the same columns.

#include "bench_common.hpp"
#include "workload.hpp"

#include "taskforge/async_pool.hpp"
#include "taskforge/false_sharing.hpp"
#include "taskforge/mutex_queue_pool.hpp"
#include "taskforge/thread_pool.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace taskforge;
using namespace taskforge::bench;

namespace {

struct Args {
  std::string bench = "granularity";
  std::string pool = "all";
  std::string threads = "";  // "" => hardware_concurrency, "sweep" => 1..hw
  std::string task_us = "";  // per-bench default when unset
  int reps = 11;
  int warmup = 3;
  std::int64_t tasks = 0;  // 0 => auto
  double imbalance = 32.0;
  bool pin = false;
  bool header = true;
  std::string out;
};

[[noreturn]] void usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s --bench <granularity|scaling|imbalanced|dag|steal-rate> "
               "[options]\nsee the comment block at the top of bench_main.cpp\n",
               prog);
  std::exit(2);
}

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) usage(argv[0]);
      return argv[++i];
    };
    if (k == "--bench") a.bench = next();
    else if (k == "--pool") a.pool = next();
    else if (k == "--threads") a.threads = next();
    else if (k == "--task-us") a.task_us = next();
    else if (k == "--reps") a.reps = std::atoi(next().c_str());
    else if (k == "--warmup") a.warmup = std::atoi(next().c_str());
    else if (k == "--tasks") a.tasks = std::atoll(next().c_str());
    else if (k == "--imbalance") a.imbalance = std::atof(next().c_str());
    else if (k == "--pin") a.pin = true;
    else if (k == "--no-header") a.header = false;
    else if (k == "--out") a.out = next();
    else if (k == "--help" || k == "-h") usage(argv[0]);
    else { std::fprintf(stderr, "unknown arg: %s\n", k.c_str()); usage(argv[0]); }
  }
  return a;
}

unsigned hw() {
  unsigned h = std::thread::hardware_concurrency();
  return h ? h : 4;
}

// Per-task microseconds: the CLI value, or `dflt` if --task-us was not given.
double task_us_or(const Args& a, double dflt) {
  if (a.task_us.empty() || a.task_us == "sweep") return dflt;
  const double v = std::atof(a.task_us.c_str());
  return v > 0 ? v : dflt;
}

unsigned threads_or_hw(const Args& a) {
  return a.threads.empty() ? hw()
                           : static_cast<unsigned>(std::atoi(a.threads.c_str()));
}

// -------------------------------------------------------------------------
// Run one (pool-type, threads, workload) configuration: reps+warmup passes of
// "submit every task, wait_idle, time it", then a CSV row of the kept reps.
// -------------------------------------------------------------------------
template <class Pool>
void run_config(CsvWriter& csv, const std::string& bench, const char* pool_name,
                unsigned threads, const Workload& wl, const BusyWork& busy,
                int reps, int warmup, bool pin) {
  std::vector<double> kept;
  kept.reserve(static_cast<std::size_t>(reps));
  const int total = reps + warmup;

  for (int r = 0; r < total; ++r) {
    Pool pool(threads);
    // Optional pinning: assign worker i to CPU i (best effort). We can only pin
    // from inside a task the worker runs, so fire one pinning task per thread
    // before timing. Not exact, but enough to reduce migration noise.
    if (pin && pinning_available()) {
      std::atomic<int> pinned{0};
      for (unsigned c = 0; c < threads; ++c)
        pool.submit([c, &pinned] { pin_this_thread(c); pinned.fetch_add(1); });
      pool.wait_idle();
    }

    const auto& d = wl.durations_us;
    const auto t0 = Clock::now();
    for (double us : d) pool.submit([&busy, us] { busy.spin(us); });
    pool.wait_idle();
    const double ms = ms_between(t0, Clock::now());
    if (r >= warmup) kept.push_back(ms);
  }

  const Summary s = Summary::of(kept);
  csv.row(bench, pool_name, threads, wl.name, wl.mean_us(), wl.size(), s,
          busy.iters_per_us(), pin && pinning_available());
}

// pool selector: "all", or a comma-separated list of
// workstealing|mutex|async.
bool pool_selected(const std::string& sel, const char* name) {
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

void run_one_pool(CsvWriter& csv, const std::string& bench, const std::string& pool,
                  unsigned threads, const Workload& wl, const BusyWork& busy, int reps,
                  int warmup, bool pin) {
  struct WS { using type = ThreadPool; };
  struct MX { using type = MutexQueuePool; };
  struct AS { using type = AsyncPool; };
  auto go = [&](const char* name, auto tag) {
    using P = typename decltype(tag)::type;
    run_config<P>(csv, bench, name, threads, wl, busy, reps, warmup, pin);
  };
  if (pool_selected(pool, "workstealing")) go("workstealing", WS{});
  if (pool_selected(pool, "mutex")) go("mutex", MX{});
  if (pool_selected(pool, "async")) go("async", AS{});
}

// -------------------------------------------------------------------------
// Sweeps
// -------------------------------------------------------------------------
void bench_granularity(const Args& a, CsvWriter& csv) {
  const BusyWork busy;
  const unsigned threads = threads_or_hw(a);
  // Log-spaced per-task sizes, ~0.25us .. ~8ms. Constant total work per point
  // (~250 ms) so wall times stay comparable and runs stay short.
  const double sizes_us[] = {0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512,
                             1024, 2048, 4096, 8192};
  const double target_total_us = 250000.0;
  for (double us : sizes_us) {
    auto n = static_cast<std::int64_t>(target_total_us / us);
    n = std::max<std::int64_t>(2000, std::min<std::int64_t>(n, 4000000));
    const Workload wl = make_uniform(n, us);
    run_one_pool(csv, "granularity", a.pool, threads, wl, busy, a.reps, a.warmup, a.pin);
  }
}

void bench_scaling(const Args& a, CsvWriter& csv) {
  const BusyWork busy;
  const double us = task_us_or(a, 8.0);
  const std::int64_t n = a.tasks ? a.tasks : 200000;
  const Workload wl = make_uniform(n, us);
  for (unsigned t = 1; t <= hw(); ++t)
    run_one_pool(csv, "scaling", a.pool, t, wl, busy, a.reps, a.warmup, a.pin);
}

void bench_imbalanced(const Args& a, CsvWriter& csv) {
  const BusyWork busy;
  const unsigned threads = threads_or_hw(a);
  const double us = task_us_or(a, 20.0);
  const std::int64_t n = a.tasks ? a.tasks : 100000;
  // Same total work, three distribution shapes.
  const Workload uniform = make_uniform(n, us);
  const Workload mixed = make_mixed(n, us, 8.0, 0.1);
  const Workload imbal = make_imbalanced(n, us, a.imbalance);
  for (const Workload* wl : {&uniform, &mixed, &imbal})
    run_one_pool(csv, "imbalanced", a.pool, threads, *wl, busy, a.reps, a.warmup, a.pin);
}

// Recursive fork-join Fibonacci: exercises help_until() and per-task spawn cost.
template <class Pool>
long long fib_pool(Pool& pool, int n, const BusyWork& busy, double leaf_us) {
  if (n < 2) {
    busy.spin(leaf_us);
    return n;
  }
  long long a = 0, b = 0;
  std::atomic<int> done{0};
  pool.submit([&] { a = fib_pool(pool, n - 1, busy, leaf_us); done.fetch_add(1, std::memory_order_release); });
  pool.submit([&] { b = fib_pool(pool, n - 2, busy, leaf_us); done.fetch_add(1, std::memory_order_release); });
  pool.help_until([&] { return done.load(std::memory_order_acquire) == 2; });
  return a + b;
}

void bench_dag(const Args& a, CsvWriter& csv) {
  const BusyWork busy;
  const int fibn = 30;          // ~1.3M nodes
  const double leaf_us = 1.0;
  const std::int64_t nodes = 2692537;  // fib(30) call-tree size, for throughput
  auto one = [&](const char* name, auto tag) {
    using P = typename decltype(tag)::type;
    std::vector<double> kept;
    for (int r = 0; r < a.reps + a.warmup; ++r) {
      P pool(threads_or_hw(a));
      const auto t0 = Clock::now();
      volatile long long sink = 0;
      pool.submit([&] { sink = fib_pool(pool, fibn, busy, leaf_us); });
      pool.wait_idle();
      (void)sink;
      const double ms = ms_between(t0, Clock::now());
      if (r >= a.warmup) kept.push_back(ms);
    }
    Summary s = Summary::of(kept);
    csv.row("dag", name, threads_or_hw(a),
            "fib30", leaf_us, nodes, s, busy.iters_per_us(), false);
  };
  struct WS { using type = ThreadPool; };
  struct MX { using type = MutexQueuePool; };
  if (pool_selected(a.pool, "workstealing")) one("workstealing", WS{});
  if (pool_selected(a.pool, "mutex")) one("mutex", MX{});
  // AsyncPool would spawn ~1.3M threads for fib(30); intentionally skipped.
}

// Divide-and-conquer parallel-for: recursively split [lo,hi), each half its own
// task pushed onto the running worker's OWN deque, parent waits via help_until.
// Unlike a flat external submit (which sends everything through the injection
// queue and never touches a deque), this genuinely exercises stealing: a worker
// whose subtree is light finishes and steals from one whose subtree is heavy.
void parfor_spin(ThreadPool& pool, const double* dur, std::size_t lo, std::size_t hi,
                 const BusyWork& busy, std::size_t grain) {
  if (hi - lo <= grain) {
    for (std::size_t i = lo; i < hi; ++i) busy.spin(dur[i]);
    return;
  }
  const std::size_t mid = lo + (hi - lo) / 2;
  std::atomic<int> done{0};
  pool.submit([&] { parfor_spin(pool, dur, lo, mid, busy, grain);
                    done.fetch_add(1, std::memory_order_release); });
  pool.submit([&] { parfor_spin(pool, dur, mid, hi, busy, grain);
                    done.fetch_add(1, std::memory_order_release); });
  pool.help_until([&] { return done.load(std::memory_order_acquire) == 2; });
}

// -------------------------------------------------------------------------
// Stage 7: steal-rate instrumentation. Runs a divide-and-conquer parallel-for
// over a heavy-tailed set of leaf durations at a range of imbalance factors,
// and dumps the work-stealing pool's PER-WORKER counters. Its own CSV schema
// (one row per worker per imbalance point); plot_steal_rate.py reads
// imbalance/worker/tasks_executed/steals_*.
// -------------------------------------------------------------------------
void bench_steal_rate(const Args& a, std::FILE* out) {
  const BusyWork busy;
  const unsigned threads =
      threads_or_hw(a);
  // Few, chunky tasks: with tens of thousands of tiny tasks the law of large
  // numbers evens every worker's load out regardless of the distribution's
  // shape. Here a single heavy-tailed task is a real fraction of a worker's
  // slice, so imbalance actually strands workers -> visible steal skew + sleep.
  const double us = task_us_or(a, 3000.0);
  const std::int64_t n = a.tasks ? a.tasks : 400;

  if (a.header) {
    std::fprintf(out,
                 "bench,pool,threads,imbalance,worker,wall_ms,"
                 "tasks_executed,own_pops,steals_success,steals_abort,"
                 "steals_empty,inject_takes,spin_iters,yield_iters,sleeps,sleep_ns\n");
  }

  const double imbs[] = {1, 2, 4, 8, 16, 32, 64, 128};
  for (double imb : imbs) {
    const Workload wl =
        imb <= 1.0 ? make_uniform(n, us) : make_imbalanced(n, us, imb);

    ThreadPool pool(threads);
    const auto t0 = Clock::now();
    // Seed EVERY leaf onto one worker's deque (a single generator task that
    // loops submit()). The other threads must steal all of it. Uniform
    // durations -> steals spread evenly and workers stay busy; heavy-tailed
    // durations -> workers that grab light tasks come back sooner (steal-count
    // skew) while early-finishers run out of stealable work and sleep. The
    // per-worker steal skew and the rise in sleep_ns with imbalance are the
    // "mechanism working / not working" signal.
    pool.submit([&] {
      for (double d : wl.durations_us) pool.submit([&busy, d] { busy.spin(d); });
    });
    pool.wait_idle();
    const double ms = ms_between(t0, Clock::now());

    const PoolStats st = pool.stats();
    for (unsigned w = 0; w < st.per_worker.size(); ++w) {
      const auto& c = st.per_worker[w];
      std::fprintf(out,
                   "steal-rate,workstealing,%u,%g,%u,%.4f,"
                   "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                   threads, imb, w, ms,
                   static_cast<unsigned long long>(c.tasks_executed),
                   static_cast<unsigned long long>(c.own_pops),
                   static_cast<unsigned long long>(c.steals_success),
                   static_cast<unsigned long long>(c.steals_abort),
                   static_cast<unsigned long long>(c.steals_empty),
                   static_cast<unsigned long long>(c.inject_takes),
                   static_cast<unsigned long long>(c.spin_iters),
                   static_cast<unsigned long long>(c.yield_iters),
                   static_cast<unsigned long long>(c.sleeps),
                   static_cast<unsigned long long>(c.sleep_ns));
    }
    std::fflush(out);
  }
}

// -------------------------------------------------------------------------
// Stage 8: the false-sharing experiment. Same counter-hammer workload, once
// with packed per-thread slots (sharing cache lines) and once with
// alignas(64) slots, sweeping thread count. Own schema with a `layout` column.
// On Linux, scripts/run_perf.sh wraps this to add cache-miss counts.
// -------------------------------------------------------------------------
void bench_false_sharing(const Args& a, std::FILE* out) {
  const std::uint64_t iters = a.tasks ? static_cast<std::uint64_t>(a.tasks)
                                      : 200000000ULL;  // per thread
  const int reps = a.reps > 0 ? a.reps : 7;
  const int warmup = a.warmup >= 0 ? a.warmup : 2;

  if (a.header) {
    std::fprintf(out,
                 "bench,layout,threads,iters_per_thread,reps,"
                 "median_ms,mean_ms,p95_ms,stddev_ms,throughput_incr_per_s\n");
  }

  for (unsigned t = 1; t <= hw(); ++t) {
    for (taskforge::FsLayout layout :
         {taskforge::FsLayout::kPacked, taskforge::FsLayout::kAligned}) {
      std::vector<double> kept;
      for (int r = 0; r < reps + warmup; ++r) {
        const double sec = taskforge::run_false_sharing(layout, t, iters);
        if (r >= warmup) kept.push_back(sec * 1000.0);
      }
      const Summary s = Summary::of(kept);
      const double thru =
          s.median_ms > 0
              ? static_cast<double>(t) * static_cast<double>(iters) / (s.median_ms / 1000.0)
              : 0.0;
      std::fprintf(out, "false-sharing,%s,%u,%llu,%zu,%.4f,%.4f,%.4f,%.4f,%.1f\n",
                   layout == taskforge::FsLayout::kPacked ? "packed" : "aligned",
                   t, static_cast<unsigned long long>(iters), s.reps, s.median_ms,
                   s.mean_ms, s.p95_ms, s.stddev_ms, thru);
      std::fflush(out);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const Args a = parse(argc, argv);

  std::FILE* out = stdout;
  if (!a.out.empty()) {
    out = std::fopen(a.out.c_str(), "ab");
    if (!out) { std::perror("fopen"); return 1; }
  }

  if (a.bench == "steal-rate") {
    bench_steal_rate(a, out);
    if (out != stdout) std::fclose(out);
    return 0;
  }
  if (a.bench == "false-sharing") {
    bench_false_sharing(a, out);
    if (out != stdout) std::fclose(out);
    return 0;
  }

  CsvWriter csv(out);
  if (a.header) csv.header();

  if (a.bench == "granularity") bench_granularity(a, csv);
  else if (a.bench == "scaling") bench_scaling(a, csv);
  else if (a.bench == "imbalanced") bench_imbalanced(a, csv);
  else if (a.bench == "dag") bench_dag(a, csv);
  else { std::fprintf(stderr, "unknown --bench %s\n", a.bench.c_str()); usage(argv[0]); }

  if (out != stdout) std::fclose(out);
  return 0;
}
