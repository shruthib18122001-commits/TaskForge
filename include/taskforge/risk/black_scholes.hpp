// black_scholes.hpp -- closed-form European option price and Greeks.
// =============================================================================
// Geometric Brownian motion, constant volatility, no dividends, no jumps, no
// smile -- the textbook Black-Scholes-Merton model. This is the reference
// everything else in the risk engine is checked against: the Monte Carlo
// engine's price should land within a few standard errors of these formulas
// (P2-1 test), and full-revaluation VaR reprices every vanilla position with
// this same closed form at the shocked spot and reduced time-to-expiry
// (P2-2). It is also the "truth" delta-normal VaR is compared against to show
// where a linear (delta-only) approximation breaks down.
//
// Preconditions on every function here, documented rather than runtime-
// checked: spot > 0, strike > 0, vol > 0, time_to_expiry > 0. This is
// hot-path numerical code called from a simulation loop, potentially millions
// of times per benchmark run; it trusts its caller the same way the rest of
// the engine trusts internal invariants instead of re-validating them, and
// the inputs it will ever see come from a portfolio built with sane financial
// parameters, not untrusted user input.
// =============================================================================

#pragma once

#include <cmath>

namespace taskforge::risk {

enum class OptionType { kCall, kPut };

struct BlackScholesInputs {
  double spot;             // S: current price of the underlying
  double strike;            // K: strike price
  double rate;              // r: continuously-compounded risk-free rate, annualized
  double vol;                // sigma: volatility, annualized
  double time_to_expiry;    // T: years remaining until expiry
};

struct OptionGreeks {
  double delta = 0.0;  // dPrice/dSpot
  double gamma = 0.0;  // d^2Price/dSpot^2 == dDelta/dSpot (same for call & put)
  double vega = 0.0;   // dPrice/dVol, per 1.0 (100 percentage points) of vol (same for call & put)
  double theta = 0.0;  // dPrice/dCalendarTime == -dPrice/dTimeToExpiry; usually negative (time decay)
  double rho = 0.0;    // dPrice/dRate
};

// Standard normal density and CDF. norm_cdf uses erfc rather than
// 0.5*(1+erf(x/sqrt2)): for large negative x, 1+erf(...) cancels two
// near-equal values and loses precision, while erfc(-x/sqrt2) does not --
// the standard numerically-stable form of this identity.
inline double norm_pdf(double x) {
  constexpr double kInvSqrt2Pi = 0.3989422804014326779399460599343818684758586311649;
  return kInvSqrt2Pi * std::exp(-0.5 * x * x);
}

inline double norm_cdf(double x) {
  constexpr double kInvSqrt2 = 0.70710678118654752440084436210485;
  return 0.5 * std::erfc(-x * kInvSqrt2);
}

namespace detail {

struct D1D2 {
  double d1, d2;
};

// d1 and d2 are shared by the price and every Greek below, so they are
// computed once here rather than re-derived in each formula.
inline D1D2 compute_d1_d2(const BlackScholesInputs& in) {
  const double sqrt_t = std::sqrt(in.time_to_expiry);
  const double d1 = (std::log(in.spot / in.strike) +
                     (in.rate + 0.5 * in.vol * in.vol) * in.time_to_expiry) /
                    (in.vol * sqrt_t);
  const double d2 = d1 - in.vol * sqrt_t;
  return {d1, d2};
}

}  // namespace detail

inline double black_scholes_price(OptionType type, const BlackScholesInputs& in) {
  const auto [d1, d2] = detail::compute_d1_d2(in);
  const double discount = std::exp(-in.rate * in.time_to_expiry);
  if (type == OptionType::kCall) {
    return in.spot * norm_cdf(d1) - in.strike * discount * norm_cdf(d2);
  }
  return in.strike * discount * norm_cdf(-d2) - in.spot * norm_cdf(-d1);
}

inline OptionGreeks black_scholes_greeks(OptionType type, const BlackScholesInputs& in) {
  const auto [d1, d2] = detail::compute_d1_d2(in);
  const double sqrt_t = std::sqrt(in.time_to_expiry);
  const double discount = std::exp(-in.rate * in.time_to_expiry);
  const double pdf_d1 = norm_pdf(d1);

  OptionGreeks g;
  // Gamma and vega are identical for calls and puts (put-call parity has
  // linear sensitivity to spot and vol cancel out of the difference).
  g.gamma = pdf_d1 / (in.spot * in.vol * sqrt_t);
  g.vega = in.spot * pdf_d1 * sqrt_t;

  if (type == OptionType::kCall) {
    g.delta = norm_cdf(d1);
    g.theta = -(in.spot * pdf_d1 * in.vol) / (2.0 * sqrt_t) -
              in.rate * in.strike * discount * norm_cdf(d2);
    g.rho = in.strike * in.time_to_expiry * discount * norm_cdf(d2);
  } else {
    g.delta = norm_cdf(d1) - 1.0;
    g.theta = -(in.spot * pdf_d1 * in.vol) / (2.0 * sqrt_t) +
              in.rate * in.strike * discount * norm_cdf(-d2);
    g.rho = -in.strike * in.time_to_expiry * discount * norm_cdf(-d2);
  }
  return g;
}

}  // namespace taskforge::risk
