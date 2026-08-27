#include "workload.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace taskforge::bench {

Workload make_uniform(std::int64_t n, double task_us) {
  Workload w;
  w.name = "uniform";
  w.durations_us.assign(static_cast<std::size_t>(n), task_us);
  return w;
}

Workload make_mixed(std::int64_t n, double short_us, double ratio, double long_frac,
                    std::uint64_t seed) {
  Workload w;
  w.name = "mixed";
  w.durations_us.resize(static_cast<std::size_t>(n));
  const auto n_long = static_cast<std::int64_t>(static_cast<double>(n) * long_frac);
  for (std::int64_t i = 0; i < n; ++i)
    w.durations_us[static_cast<std::size_t>(i)] = (i < n_long) ? short_us * ratio : short_us;
  std::mt19937_64 rng(seed);
  std::shuffle(w.durations_us.begin(), w.durations_us.end(), rng);
  return w;
}

Workload make_imbalanced(std::int64_t n, double task_us, double imbalance_factor,
                         std::uint64_t seed) {
  Workload w;
  w.name = "imbalanced";
  w.durations_us.resize(static_cast<std::size_t>(n));

  // Log-normal: pick sigma so that exp(2.33 * sigma) ~= imbalance_factor
  // (2.33 ~ the 99th-percentile z-score), i.e. p99/median ~= imbalance_factor.
  const double sigma = std::log(std::max(1.0001, imbalance_factor)) / 2.33;
  const double mu = 0.0;  // rescaled below to hit the target mean exactly
  std::mt19937_64 rng(seed);
  std::lognormal_distribution<double> dist(mu, sigma);

  double sum = 0;
  for (std::int64_t i = 0; i < n; ++i) {
    const double s = dist(rng);
    w.durations_us[static_cast<std::size_t>(i)] = s;
    sum += s;
  }
  // Rescale so total work == n * task_us (same budget as make_uniform), letting
  // us attribute any speed difference to distribution shape, not total load.
  const double target_total = static_cast<double>(n) * task_us;
  const double k = (sum > 0) ? target_total / sum : 1.0;
  for (double& d : w.durations_us) d *= k;
  return w;
}

}  // namespace taskforge::bench
