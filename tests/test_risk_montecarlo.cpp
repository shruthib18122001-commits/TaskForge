// Phase 2 / P2-1 -- Monte Carlo price within standard error of closed form.
//
// A tiny, serial, test-local European-option Monte Carlo pricer (NOT the
// production engine -- that's engine.hpp in P2-2/P2-3) that exists to prove
// rng.hpp's normal draws, combined with the standard GBM terminal-price
// formula, actually reproduce the Black-Scholes closed form. This is the
// single test that exercises rng.hpp and black_scholes.hpp together.

#include "taskforge/risk/black_scholes.hpp"
#include "taskforge/risk/rng.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

using taskforge::risk::BlackScholesInputs;
using taskforge::risk::black_scholes_price;
using taskforge::risk::chunk_seed;
using taskforge::risk::OptionType;
using taskforge::risk::RngStream;

namespace {

struct McResult {
  double price = 0.0;
  double standard_error = 0.0;
};

// Risk-neutral GBM terminal price: S_T = S0 * exp((r - sigma^2/2)T + sigma*sqrt(T)*Z).
// Pricing uses the risk-neutral drift r (not the real-world drift P2-2's VaR
// simulation will use) -- that's what makes this a *pricing* Monte Carlo and
// discounting at r a valid estimator of today's fair value.
McResult mc_price(OptionType type, const BlackScholesInputs& in, std::int64_t n_paths,
                  std::uint64_t seed) {
  RngStream rng(seed);
  const double drift = (in.rate - 0.5 * in.vol * in.vol) * in.time_to_expiry;
  const double diffusion = in.vol * std::sqrt(in.time_to_expiry);
  const double discount = std::exp(-in.rate * in.time_to_expiry);

  double sum = 0.0, sum_sq = 0.0;
  for (std::int64_t i = 0; i < n_paths; ++i) {
    const double z = rng.next_standard_normal();
    const double s_t = in.spot * std::exp(drift + diffusion * z);
    const double payoff = type == OptionType::kCall ? std::max(s_t - in.strike, 0.0)
                                                    : std::max(in.strike - s_t, 0.0);
    const double discounted = discount * payoff;
    sum += discounted;
    sum_sq += discounted * discounted;
  }
  const double n = static_cast<double>(n_paths);
  const double mean = sum / n;
  const double variance = sum_sq / n - mean * mean;

  McResult r;
  r.price = mean;
  r.standard_error = std::sqrt(variance / n);
  return r;
}

}  // namespace

TEST(RiskMonteCarlo, PriceWithinStandardErrorOfClosedForm) {
  const BlackScholesInputs in{100.0, 100.0, 0.05, 0.20, 1.0};
  const std::int64_t n_paths = 2'000'000;
  // 4 standard errors, per the Phase 2 spec's own example tolerance. With a
  // correct estimator this should essentially never flake (P(|Z|>4) ~ 6e-5
  // per run, and this one test runs once per CI invocation).
  const int kSeFactor = 4;

  for (OptionType t : {OptionType::kCall, OptionType::kPut}) {
    const McResult mc = mc_price(t, in, n_paths, chunk_seed(0xBEEF, static_cast<std::uint64_t>(t)));
    const double closed_form = black_scholes_price(t, in);
    EXPECT_NEAR(mc.price, closed_form, kSeFactor * mc.standard_error)
        << "type=" << (t == OptionType::kCall ? "call" : "put")
        << " mc=" << mc.price << " closed_form=" << closed_form
        << " se=" << mc.standard_error;
  }
}

TEST(RiskMonteCarlo, SameSeedGivesBitIdenticalPrice) {
  // Not a statistical check -- a literal determinism check on the pricer
  // itself, foreshadowing the chunk-reduction determinism tests in P2-3.
  const BlackScholesInputs in{100.0, 100.0, 0.05, 0.20, 1.0};
  const McResult a = mc_price(OptionType::kCall, in, 50000, 999);
  const McResult b = mc_price(OptionType::kCall, in, 50000, 999);
  EXPECT_EQ(a.price, b.price);
  EXPECT_EQ(a.standard_error, b.standard_error);
}
