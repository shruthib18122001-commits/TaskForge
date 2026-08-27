// workload.hpp -- the task-duration profiles the harness runs.
//
// A Workload is just a list of per-task durations in microseconds. The runner
// submits one task per entry; task i calls BusyWork::spin(durations[i]). Keeping
// it this concrete makes every workload reproducible (fixed RNG seed) and makes
// "imbalanced" mean something precise rather than hand-wavy.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace taskforge::bench {

struct Workload {
  std::string name;
  std::vector<double> durations_us;  // one entry per task

  std::int64_t size() const { return static_cast<std::int64_t>(durations_us.size()); }
  double total_us() const {
    double s = 0;
    for (double d : durations_us) s += d;
    return s;
  }
  double mean_us() const {
    return durations_us.empty() ? 0.0 : total_us() / static_cast<double>(durations_us.size());
  }
};

// n tasks, every one exactly `task_us`. The granularity and scaling sweeps use
// this; the sweep varies task_us (granularity) or thread count (scaling).
Workload make_uniform(std::int64_t n, double task_us);

// n tasks: (1-long_frac) of them at `short_us`, long_frac of them at
// `short_us * ratio`, shuffled. Same code path as uniform when ratio == 1.
Workload make_mixed(std::int64_t n, double short_us, double ratio, double long_frac,
                    std::uint64_t seed = 0xB0BAFE77ULL);

// Deliberately uneven: durations drawn from a heavy-tailed (log-normal-ish)
// distribution with the given imbalance factor (ratio of the ~99th percentile
// to the median), same total work as make_uniform(n, task_us). This is where
// work stealing should look best.
Workload make_imbalanced(std::int64_t n, double task_us, double imbalance_factor,
                         std::uint64_t seed = 0x5EED1234ULL);

}  // namespace taskforge::bench
