// bench_common.hpp -- timing, summary statistics, CSV emission, CPU pinning.
//
// Rules the harness follows (from the project spec):
//   * std::chrono::steady_clock for wall time.
//   * Discard warmup repetitions.
//   * Report median, mean, p95, p99, and stddev across the kept repetitions.
//   * Pin worker threads where it reduces noise, and record whether pinning
//     was on in the CSV.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#if defined(TF_HAVE_SETAFFINITY)
#include <pthread.h>
#include <sched.h>
#endif

namespace taskforge::bench {

using Clock = std::chrono::steady_clock;

inline double ms_between(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

// ---------------------------------------------------------------------------
// Calibrated busy-work: spin for approximately `microseconds` of CPU time.
// Calibrated once at startup by timing a fixed amount of the same spin. This
// is deliberately simple; it drifts with turbo/thermal/scheduling and is only
// meant to set task granularity to the right order of magnitude. The harness
// records the calibration in the CSV so a reader can judge it.
// ---------------------------------------------------------------------------
class BusyWork {
 public:
  BusyWork() { calibrate(); }

  // Spin ~us microseconds. `sink` defeats dead-code elimination.
  void spin(double us) const {
    const std::uint64_t iters = static_cast<std::uint64_t>(us * iters_per_us_);
    std::uint64_t acc = seed_;
    for (std::uint64_t i = 0; i < iters; ++i) {
      acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;  // LCG step
      acc ^= acc >> 17;
    }
    sink_.store(acc, std::memory_order_relaxed);
  }

  double iters_per_us() const { return iters_per_us_; }

 private:
  void calibrate() {
    // Ramp iteration count until a run takes >= 20 ms, then derive the rate.
    std::uint64_t iters = 1u << 16;
    for (;;) {
      const auto t0 = Clock::now();
      std::uint64_t acc = 0x9E3779B97F4A7C15ULL;
      for (std::uint64_t i = 0; i < iters; ++i) {
        acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
        acc ^= acc >> 17;
      }
      sink_.store(acc, std::memory_order_relaxed);
      const double ms = ms_between(t0, Clock::now());
      if (ms >= 20.0) {
        iters_per_us_ = static_cast<double>(iters) / (ms * 1000.0);
        return;
      }
      iters <<= 1;
    }
  }

  double iters_per_us_ = 1.0;
  std::uint64_t seed_ = 0x1234567ABCDEF01ULL;
  mutable std::atomic<std::uint64_t> sink_{0};
};

// ---------------------------------------------------------------------------
// Summary statistics over a set of per-repetition wall times (milliseconds).
// ---------------------------------------------------------------------------
struct Summary {
  std::size_t reps = 0;
  double median_ms = 0, mean_ms = 0, p95_ms = 0, p99_ms = 0, stddev_ms = 0;
  double min_ms = 0, max_ms = 0;

  static double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p / 100.0 * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(idx));
    const auto hi = static_cast<std::size_t>(std::ceil(idx));
    const double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
  }

  static Summary of(const std::vector<double>& raw) {
    Summary s;
    s.reps = raw.size();
    if (raw.empty()) return s;
    double sum = 0;
    for (double x : raw) sum += x;
    s.mean_ms = sum / static_cast<double>(raw.size());
    double var = 0;
    for (double x : raw) var += (x - s.mean_ms) * (x - s.mean_ms);
    s.stddev_ms = std::sqrt(var / static_cast<double>(raw.size()));
    s.median_ms = percentile(raw, 50);
    s.p95_ms = percentile(raw, 95);
    s.p99_ms = percentile(raw, 99);
    auto mm = std::minmax_element(raw.begin(), raw.end());
    s.min_ms = *mm.first;
    s.max_ms = *mm.second;
    return s;
  }
};

// ---------------------------------------------------------------------------
// Best-effort pin of the calling thread to `cpu`. Linux only; a no-op (returns
// false) elsewhere. bench_main records whether pinning was requested/available.
// ---------------------------------------------------------------------------
inline bool pin_this_thread(unsigned cpu) {
#if defined(TF_HAVE_SETAFFINITY)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)cpu;
  return false;
#endif
}

inline bool pinning_available() {
#if defined(TF_HAVE_SETAFFINITY)
  return true;
#else
  return false;
#endif
}

// ---------------------------------------------------------------------------
// CSV writer. One flat schema for every sweep so results concatenate cleanly.
//   bench,pool,threads,workload,task_us,n_tasks,reps,
//   median_ms,mean_ms,p95_ms,p99_ms,stddev_ms,min_ms,max_ms,
//   throughput_tasks_per_s,iters_per_us,pinned
// ---------------------------------------------------------------------------
class CsvWriter {
 public:
  explicit CsvWriter(std::FILE* out) : out_(out) {}

  void header() {
    std::fprintf(out_,
                 "bench,pool,threads,workload,task_us,n_tasks,reps,"
                 "median_ms,mean_ms,p95_ms,p99_ms,stddev_ms,min_ms,max_ms,"
                 "throughput_tasks_per_s,iters_per_us,pinned\n");
  }

  void row(const std::string& bench, const std::string& pool, unsigned threads,
           const std::string& workload, double task_us, std::int64_t n_tasks,
           const Summary& s, double iters_per_us, bool pinned) {
    const double thru = s.median_ms > 0
                            ? static_cast<double>(n_tasks) / (s.median_ms / 1000.0)
                            : 0.0;
    std::fprintf(out_,
                 "%s,%s,%u,%s,%.4f,%lld,%zu,"
                 "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,"
                 "%.2f,%.2f,%d\n",
                 bench.c_str(), pool.c_str(), threads, workload.c_str(), task_us,
                 static_cast<long long>(n_tasks), s.reps, s.median_ms, s.mean_ms,
                 s.p95_ms, s.p99_ms, s.stddev_ms, s.min_ms, s.max_ms, thru,
                 iters_per_us, pinned ? 1 : 0);
    std::fflush(out_);
  }

 private:
  std::FILE* out_;
};

}  // namespace taskforge::bench
