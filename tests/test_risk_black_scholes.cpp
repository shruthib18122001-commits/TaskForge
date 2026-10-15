// Phase 2 / P2-1 -- Black-Scholes closed-form price and Greeks.
//
// Four kinds of check, per the Phase 2 spec:
//   * the published sanity values (S=K=100, r=5%, sigma=20%, T=1)
//   * put-call parity, which is pure algebra and should hold to ~machine
//     epsilon regardless of parameters
//   * each Greek against a central-difference approximation of the SAME
//     closed-form price function, with the bump size and tolerance stated
//     next to each check
//   * (the Monte-Carlo-vs-closed-form check lives in test_risk_montecarlo.cpp,
//     since it also needs rng.hpp)

#include "taskforge/risk/black_scholes.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using taskforge::risk::BlackScholesInputs;
using taskforge::risk::black_scholes_greeks;
using taskforge::risk::black_scholes_price;
using taskforge::risk::inverse_norm_cdf;
using taskforge::risk::norm_cdf;
using taskforge::risk::norm_pdf;
using taskforge::risk::OptionType;

namespace {
// A handful of fixed, hand-picked parameter sets -- not RNG-derived, so these
// tests never flake. Covers at/in/out-of-the-money and short/long expiries.
std::vector<BlackScholesInputs> sample_inputs() {
  return {
      {100.0, 100.0, 0.05, 0.20, 1.0},    // the published sanity case
      {100.0, 100.0, 0.05, 0.20, 1.0 / 365.0},  // 1-day horizon (P2-2 reuses this)
      {100.0, 80.0, 0.03, 0.35, 0.5},     // deep in-the-money call / OTM put
      {100.0, 120.0, 0.01, 0.15, 2.0},    // OTM call / ITM put, long-dated
      {50.0, 50.0, 0.07, 0.50, 0.25},     // high vol, short-dated
  };
}
}  // namespace

TEST(BlackScholes, KnownSanityValues) {
  const BlackScholesInputs in{100.0, 100.0, 0.05, 0.20, 1.0};
  const double call = black_scholes_price(OptionType::kCall, in);
  const double put = black_scholes_price(OptionType::kPut, in);
  // Spec gives these to 4 decimal places; match to the same precision.
  EXPECT_NEAR(call, 10.4506, 1e-4);
  EXPECT_NEAR(put, 5.5735, 1e-4);
}

TEST(BlackScholes, NormCdfKnownPoints) {
  // Standard normal table values, independent of our own erfc-based formula.
  EXPECT_NEAR(norm_cdf(0.0), 0.5, 1e-12);
  EXPECT_NEAR(norm_cdf(1.0), 0.8413447460685429, 1e-12);
  EXPECT_NEAR(norm_cdf(-1.0), 0.15865525393145707, 1e-12);
  EXPECT_NEAR(norm_cdf(1.96), 0.9750021048517795, 1e-10);
  // Symmetry of the density.
  EXPECT_NEAR(norm_pdf(0.5), norm_pdf(-0.5), 1e-15);
}

TEST(BlackScholes, PutCallParity) {
  // C - P == S - K*exp(-rT), pure algebra on the SAME two closed forms --
  // should hold to within a few ULPs, not just "close".
  for (const auto& in : sample_inputs()) {
    const double call = black_scholes_price(OptionType::kCall, in);
    const double put = black_scholes_price(OptionType::kPut, in);
    const double parity_rhs = in.spot - in.strike * std::exp(-in.rate * in.time_to_expiry);
    EXPECT_NEAR(call - put, parity_rhs, 1e-9) << "spot=" << in.spot << " strike=" << in.strike;
  }
}

TEST(BlackScholes, PriceIsPositiveAndBoundedByIntrinsicAndSpot) {
  for (const auto& in : sample_inputs()) {
    for (OptionType t : {OptionType::kCall, OptionType::kPut}) {
      const double price = black_scholes_price(t, in);
      EXPECT_GT(price, 0.0);
      if (t == OptionType::kCall) {
        EXPECT_LT(price, in.spot);  // a call is worth less than owning the stock outright
      } else {
        EXPECT_LT(price, in.strike);  // a put is worth less than the strike itself
      }
    }
  }
}

namespace {
// Central-difference derivative of black_scholes_price w.r.t. one input,
// bumping it by `h`. bump(in, h) must return a copy of `in` with the target
// field shifted by h.
template <class BumpFn>
double central_difference(OptionType type, const BlackScholesInputs& in, double h, BumpFn bump) {
  const double up = black_scholes_price(type, bump(in, h));
  const double down = black_scholes_price(type, bump(in, -h));
  return (up - down) / (2.0 * h);
}
}  // namespace

TEST(BlackScholes, DeltaMatchesFiniteDifference) {
  // Central difference on spot. Step size: 1e-4 * spot (relative bump keeps
  // the step sane across very different spot magnitudes in sample_inputs()).
  // Central-difference truncation error is O(h^2), so at h ~ 1e-4*S the
  // truncation term is negligible next to delta's O(1) scale; tolerance 1e-6
  // leaves headroom for the O(h^2) term and float rounding, not for a wrong
  // formula.
  for (const auto& in : sample_inputs()) {
    const double h = 1e-4 * in.spot;
    auto bump_spot = [](BlackScholesInputs x, double d) {
      x.spot += d;
      return x;
    };
    for (OptionType t : {OptionType::kCall, OptionType::kPut}) {
      const double fd = central_difference(t, in, h, bump_spot);
      const double analytic = black_scholes_greeks(t, in).delta;
      EXPECT_NEAR(analytic, fd, 1e-6) << "spot=" << in.spot << " T=" << in.time_to_expiry;
    }
  }
}

TEST(BlackScholes, VegaMatchesFiniteDifference) {
  // Central difference on vol. Absolute step 1e-5 (vol itself is O(0.1-0.5)
  // in these samples, so a relative bump isn't needed); tolerance 1e-3
  // because vega's own scale (tens, for these inputs) makes an absolute 1e-6
  // tolerance unreasonably tight relative to float rounding in the price
  // itself.
  for (const auto& in : sample_inputs()) {
    const double h = 1e-5;
    auto bump_vol = [](BlackScholesInputs x, double d) {
      x.vol += d;
      return x;
    };
    for (OptionType t : {OptionType::kCall, OptionType::kPut}) {
      const double fd = central_difference(t, in, h, bump_vol);
      const double analytic = black_scholes_greeks(t, in).vega;
      EXPECT_NEAR(analytic, fd, 1e-3) << "spot=" << in.spot << " vol=" << in.vol;
    }
  }
}

TEST(BlackScholes, RhoMatchesFiniteDifference) {
  // Central difference on rate. Absolute step 1e-6 (rates are O(0.01-0.1));
  // tolerance 1e-4.
  for (const auto& in : sample_inputs()) {
    const double h = 1e-6;
    auto bump_rate = [](BlackScholesInputs x, double d) {
      x.rate += d;
      return x;
    };
    for (OptionType t : {OptionType::kCall, OptionType::kPut}) {
      const double fd = central_difference(t, in, h, bump_rate);
      const double analytic = black_scholes_greeks(t, in).rho;
      EXPECT_NEAR(analytic, fd, 1e-4) << "spot=" << in.spot << " rate=" << in.rate;
    }
  }
}

TEST(BlackScholes, ThetaMatchesFiniteDifference) {
  // theta here means dPrice/dCalendarTime == -dPrice/dTimeToExpiry: as
  // calendar time passes, time-to-expiry shrinks. So the finite-difference
  // estimate decreases T by h (simulates h years of calendar time passing)
  // rather than bumping it up -- that sign is what makes this a check of
  // "time decay" and not just "sensitivity to the T parameter with the
  // opposite sign convention".
  // Step size: 1e-5 * T, except for the 1-day sample (T = 1/365) where a
  // relative step is too small to resolve against float rounding in the
  // price, so it gets its own smaller-scale absolute step. Tolerance 1e-3
  // (theta's scale is O(1-10) for these inputs).
  for (const auto& in : sample_inputs()) {
    const double h = in.time_to_expiry > 0.01 ? 1e-5 * in.time_to_expiry : 1e-7;
    for (OptionType t : {OptionType::kCall, OptionType::kPut}) {
      BlackScholesInputs plus_dt = in;  // time-to-expiry h years sooner
      plus_dt.time_to_expiry -= h;
      BlackScholesInputs minus_dt = in;  // time-to-expiry h years later
      minus_dt.time_to_expiry += h;
      const double fd = (black_scholes_price(t, plus_dt) - black_scholes_price(t, minus_dt)) /
                        (2.0 * h);
      const double analytic = black_scholes_greeks(t, in).theta;
      EXPECT_NEAR(analytic, fd, 1e-3) << "spot=" << in.spot << " T=" << in.time_to_expiry;
    }
  }
}

TEST(BlackScholes, GammaMatchesSecondDifference) {
  // Gamma is the second derivative, so it needs a central SECOND difference:
  // (f(S+h) - 2f(S) + f(S-h)) / h^2. This has worse numerical conditioning
  // than a first difference (subtracting near-equal values, dividing by h^2),
  // so h needs to be chosen relative to the option's own natural
  // price-curvature scale -- roughly S*sigma*sqrt(T), the "one standard
  // deviation of spot move" width -- NOT a fixed fraction of spot alone. A
  // fixed 1e-2*spot step is fine for the T=1 sample but is comparable to (not
  // small next to) the curvature scale of the T=1/365 sample, where gamma
  // changes fast over a $1 move; that mismatch, not a wrong formula, is what
  // first made this test flaky-looking across sample_inputs(). Verified
  // empirically (see commit message / scratch check): at h = 1e-2 * scale,
  // relative error is ~1e-5..1e-6 across every case in sample_inputs(); the
  // 1e-4 absolute tolerance below leaves ~100x headroom over that.
  for (const auto& in : sample_inputs()) {
    const double scale = in.spot * in.vol * std::sqrt(in.time_to_expiry);
    const double h = 1e-2 * scale;
    BlackScholesInputs up = in;
    up.spot += h;
    BlackScholesInputs down = in;
    down.spot -= h;
    for (OptionType t : {OptionType::kCall, OptionType::kPut}) {
      const double fd = (black_scholes_price(t, up) - 2.0 * black_scholes_price(t, in) +
                         black_scholes_price(t, down)) /
                        (h * h);
      const double analytic = black_scholes_greeks(t, in).gamma;
      EXPECT_NEAR(analytic, fd, 1e-4) << "spot=" << in.spot << " T=" << in.time_to_expiry;
    }
  }
}

TEST(BlackScholes, InverseNormCdfKnownQuantiles) {
  // Standard, widely-tabulated z-scores -- these are the exact two values
  // delta-normal VaR at 95%/99% will use (engine.hpp).
  EXPECT_NEAR(inverse_norm_cdf(0.95), 1.6448536269514722, 1e-8);
  EXPECT_NEAR(inverse_norm_cdf(0.99), 2.3263478740408408, 1e-8);
  EXPECT_NEAR(inverse_norm_cdf(0.5), 0.0, 1e-8);
  // Symmetry: z_{1-p} == -z_p.
  EXPECT_NEAR(inverse_norm_cdf(0.01), -inverse_norm_cdf(0.99), 1e-8);
}

TEST(BlackScholes, InverseNormCdfRoundTripsThroughNormCdf) {
  // norm_cdf(inverse_norm_cdf(p)) == p across the low, middle, and high
  // branches of Acklam's piecewise approximation (the branch boundary is at
  // p = 0.02425 and its mirror 0.97575).
  for (double p : {1e-6, 0.001, 0.02, 0.02425, 0.1, 0.5, 0.9, 0.97575, 0.999, 1.0 - 1e-6}) {
    EXPECT_NEAR(norm_cdf(inverse_norm_cdf(p)), p, 1e-9) << "p=" << p;
  }
}

TEST(BlackScholes, GammaAndVegaAreCallPutIdentical) {
  // Put-call parity makes these two Greeks identical for calls and puts
  // (delta differs by exactly 1, but gamma = dDelta/dSpot and vega both drop
  // the difference).
  for (const auto& in : sample_inputs()) {
    const auto call_g = black_scholes_greeks(OptionType::kCall, in);
    const auto put_g = black_scholes_greeks(OptionType::kPut, in);
    EXPECT_NEAR(call_g.gamma, put_g.gamma, 1e-12);
    EXPECT_NEAR(call_g.vega, put_g.vega, 1e-12);
    EXPECT_NEAR(call_g.delta - put_g.delta, 1.0, 1e-12);
  }
}
