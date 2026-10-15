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

// Inverse standard normal CDF (the normal quantile function): given a
// probability p in (0, 1), returns z such that norm_cdf(z) == p.
//
// Used by delta-normal VaR/ES (engine.hpp) to turn a confidence level alpha
// (0.95, 0.99, ...) into the z-score that scales a linear portfolio's
// Gaussian standard deviation into a loss quantile. Only two fixed alphas are
// actually needed (95% and 99%), but hardcoding their z-scores as magic
// numbers would hide where those numbers come from and couldn't be checked
// against anything; a general, tested quantile function is barely more code
// and is the more honest choice.
//
// Implementation: Peter Acklam's rational (minimax) approximation
// (algorithm public domain; see Acklam, "An algorithm for computing the
// inverse normal cumulative distribution function", 2003). Relative error is
// stated to be below 1.15e-9 across the whole (0,1) domain -- far tighter
// than anything a Monte Carlo VaR estimate with finite N can resolve, so one
// application of the rational approximation (no Newton/Halley refinement
// step) is sufficient here.
inline double inverse_norm_cdf(double p) {
  constexpr double a1 = -3.969683028665376e+01, a2 = 2.209460984245205e+02,
                   a3 = -2.759285104469687e+02, a4 = 1.383577518672690e+02,
                   a5 = -3.066479806614716e+01, a6 = 2.506628277459239e+00;
  constexpr double b1 = -5.447609879822406e+01, b2 = 1.615858368580409e+02,
                   b3 = -1.556989798598866e+02, b4 = 6.680131188771972e+01,
                   b5 = -1.328068155288572e+01;
  constexpr double c1 = -7.784894002430293e-03, c2 = -3.223964580411365e-01,
                   c3 = -2.400758277161838e+00, c4 = -2.549732539343734e+00,
                   c5 = 4.374664141464968e+00, c6 = 2.938163982698783e+00;
  constexpr double d1 = 7.784695709041462e-03, d2 = 3.224671290700398e-01,
                   d3 = 2.445134137142996e+00, d4 = 3.754408661907416e+00;
  constexpr double kPLow = 0.02425;

  if (p < kPLow) {
    const double q = std::sqrt(-2.0 * std::log(p));
    return (((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
           ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
  }
  if (p <= 1.0 - kPLow) {
    const double q = p - 0.5;
    const double r = q * q;
    return (((((a1 * r + a2) * r + a3) * r + a4) * r + a5) * r + a6) * q /
           (((((b1 * r + b2) * r + b3) * r + b4) * r + b5) * r + 1.0);
  }
  const double q = std::sqrt(-2.0 * std::log(1.0 - p));
  return -(((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
         ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
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
