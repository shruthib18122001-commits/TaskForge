// verify_risk.cpp -- Phase 2 / P2-1, framework-free mirror of
// tests/test_risk_{black_scholes,rng,montecarlo}.cpp. Picked up automatically
// by the Makefile's `wildcard verify/verify_*.cpp`.

#include "taskforge/risk/black_scholes.hpp"
#include "taskforge/risk/rng.hpp"

#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

#include "../verify/tf_verify.hpp"

using namespace taskforge::risk;

static void known_sanity_values() {
  const BlackScholesInputs in{100.0, 100.0, 0.05, 0.20, 1.0};
  const double call = black_scholes_price(OptionType::kCall, in);
  const double put = black_scholes_price(OptionType::kPut, in);
  TF_CHECK_MSG(std::fabs(call - 10.4506) < 1e-4, "call=" + std::to_string(call));
  TF_CHECK_MSG(std::fabs(put - 5.5735) < 1e-4, "put=" + std::to_string(put));
}

static void put_call_parity() {
  const std::vector<BlackScholesInputs> cases = {
      {100.0, 100.0, 0.05, 0.20, 1.0},
      {100.0, 80.0, 0.03, 0.35, 0.5},
      {100.0, 120.0, 0.01, 0.15, 2.0},
  };
  for (const auto& in : cases) {
    const double call = black_scholes_price(OptionType::kCall, in);
    const double put = black_scholes_price(OptionType::kPut, in);
    const double rhs = in.spot - in.strike * std::exp(-in.rate * in.time_to_expiry);
    TF_CHECK_MSG(std::fabs((call - put) - rhs) < 1e-9,
                "parity off by " + std::to_string((call - put) - rhs));
  }
}

static void delta_matches_finite_difference() {
  const BlackScholesInputs in{100.0, 100.0, 0.05, 0.20, 1.0};
  const double h = 1e-4 * in.spot;
  BlackScholesInputs up = in;
  up.spot += h;
  BlackScholesInputs down = in;
  down.spot -= h;
  const double fd =
      (black_scholes_price(OptionType::kCall, up) - black_scholes_price(OptionType::kCall, down)) /
      (2.0 * h);
  const double analytic = black_scholes_greeks(OptionType::kCall, in).delta;
  TF_CHECK_MSG(std::fabs(fd - analytic) < 1e-6,
              "delta fd=" + std::to_string(fd) + " analytic=" + std::to_string(analytic));
}

static void rng_moments_and_tail() {
  RngStream rng(chunk_seed(999, 0));
  const int n = 200000;
  double sum = 0.0, sum_sq = 0.0;
  int above_1_645 = 0;
  for (int i = 0; i < n; ++i) {
    const double z = rng.next_standard_normal();
    sum += z;
    sum_sq += z * z;
    if (z > 1.645) ++above_1_645;
  }
  const double mean = sum / n;
  const double var = sum_sq / n - mean * mean;
  const double mean_se = 1.0 / std::sqrt(static_cast<double>(n));
  const double var_se = std::sqrt(2.0 / n);
  TF_CHECK_MSG(std::fabs(mean) < 5 * mean_se, "normal mean=" + std::to_string(mean));
  TF_CHECK_MSG(std::fabs(var - 1.0) < 5 * var_se, "normal var=" + std::to_string(var));
  const double tail_frac = static_cast<double>(above_1_645) / n;
  const double tail_se = std::sqrt(0.05 * 0.95 / n);
  TF_CHECK_MSG(std::fabs(tail_frac - 0.05) < 5 * tail_se,
              "tail frac=" + std::to_string(tail_frac));
}

static void determinism() {
  RngStream a(555), b(555);
  for (int i = 0; i < 500; ++i) {
    TF_CHECK_EQ(a.next_standard_normal(), b.next_standard_normal());
  }

  // chunk_seed: pure, order-independent, no collisions in a realistic range.
  const std::uint64_t gseed = 0xABCDEFULL;
  std::vector<std::uint64_t> seeds;
  for (std::uint64_t i = 0; i < 2000; ++i) seeds.push_back(chunk_seed(gseed, i));
  for (std::uint64_t i = 0; i < 2000; ++i) {
    TF_CHECK_EQ(chunk_seed(gseed, i), seeds[i]);  // recompute, must match (pure function)
  }
  std::set<std::uint64_t> distinct(seeds.begin(), seeds.end());
  TF_CHECK_EQ(distinct.size(), seeds.size());
}

static void monte_carlo_matches_closed_form() {
  const BlackScholesInputs in{100.0, 100.0, 0.05, 0.20, 1.0};
  const std::int64_t n_paths = 2000000;
  RngStream rng(chunk_seed(0xF00D, 1));
  const double drift = (in.rate - 0.5 * in.vol * in.vol) * in.time_to_expiry;
  const double diffusion = in.vol * std::sqrt(in.time_to_expiry);
  const double discount = std::exp(-in.rate * in.time_to_expiry);
  double sum = 0.0, sum_sq = 0.0;
  for (std::int64_t i = 0; i < n_paths; ++i) {
    const double z = rng.next_standard_normal();
    const double s_t = in.spot * std::exp(drift + diffusion * z);
    const double discounted = discount * std::max(s_t - in.strike, 0.0);
    sum += discounted;
    sum_sq += discounted * discounted;
  }
  const double n = static_cast<double>(n_paths);
  const double mean = sum / n;
  const double se = std::sqrt((sum_sq / n - mean * mean) / n);
  const double closed_form = black_scholes_price(OptionType::kCall, in);
  TF_CHECK_MSG(std::fabs(mean - closed_form) < 4 * se,
              "mc=" + std::to_string(mean) + " closed_form=" + std::to_string(closed_form) +
                  " se=" + std::to_string(se));
}

int main() {
  known_sanity_values();
  put_call_parity();
  delta_matches_finite_difference();
  rng_moments_and_tail();
  determinism();
  monte_carlo_matches_closed_form();
  TF_PASS("verify_risk");
}
