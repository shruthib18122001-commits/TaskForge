// engine.hpp -- P2-2: serial full-revaluation VaR/ES, and delta-normal VaR/ES
// for comparison.
// =============================================================================
// simulate_chunk() is the one place the GBM-plus-full-revaluation math lives.
// run_serial() calls it once, for the whole scenario range, single-threaded:
// the simplest possible ground truth, used to validate the finance before any
// parallelism enters the picture. P2-3 calls the SAME simulate_chunk() over
// many sub-ranges submitted to a thread pool; P2-4 calls it from DAG nodes.
// None of those callers duplicate the math -- they only change how the
// [0, n_scenarios) range is split up and executed.
//
// Determinism note: run_serial() treats the whole range as chunk index 0, so
// it will NOT produce bit-identical losses to P2-3's parallel engine running
// the same N with multiple chunks (different chunk boundaries -> different
// per-chunk seeds -> different draws covering different sub-ranges). That is
// expected, not a bug: P2-2's job is to validate the FINANCIAL correctness of
// the simulation (does it produce sane VaR/ES numbers), not to match P2-3 bit
// for bit. P2-3's own determinism guarantee -- same answer across thread
// counts and pools -- is a separate, narrower claim about ITS chunking,
// proven in test_risk_engine_parallel.cpp.
// =============================================================================

#pragma once

#include "taskforge/risk/black_scholes.hpp"
#include "taskforge/risk/portfolio.hpp"
#include "taskforge/risk/rng.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace taskforge::risk {

// Sentinel for "all desks" in the functions below that take an optional desk
// filter (std::size_t can't hold a signed -1, so this spells out the intent).
constexpr std::size_t kAllDesks = static_cast<std::size_t>(-1);

struct ScenarioParams {
  double horizon_years;
  double drift = 0.0;              // real-world drift mu; 0 unless stated otherwise
  std::int64_t n_scenarios;
  std::uint64_t seed;
  std::int64_t chunk_size = 1000;  // scenarios per chunk; P2-3's parallel engine reads this
};

struct QuantileResult {
  double var = 0.0;
  double es = 0.0;
};

// ---------------------------------------------------------------------------
// Each position's value AT TODAY'S SPOT (t=0), computed once per run and
// passed by const-ref into every chunk. Portfolio stays pure data (see its
// header comment); this is the per-run derived value that would otherwise
// have to be a mutable cache on Portfolio.
// ---------------------------------------------------------------------------
struct PositionBaseValues {
  std::vector<double> stocks;   // same size/order as Portfolio::stocks
  std::vector<double> options;  // same size/order as Portfolio::options
};

inline PositionBaseValues compute_base_values(const Portfolio& portfolio) {
  PositionBaseValues base;
  base.stocks.reserve(portfolio.stocks.size());
  for (const auto& s : portfolio.stocks) {
    base.stocks.push_back(s.quantity * portfolio.underlyings[s.underlying].spot);
  }
  base.options.reserve(portfolio.options.size());
  for (const auto& o : portfolio.options) {
    const Underlying& u = portfolio.underlyings[o.underlying];
    const BlackScholesInputs in{u.spot, o.strike, portfolio.rate, u.vol, o.time_to_expiry};
    base.options.push_back(o.quantity * black_scholes_price(o.type, in));
  }
  return base;
}

namespace detail {
// Value of a vanilla option at expiry (or at a horizon past its original
// expiry), i.e. pure intrinsic value. simulate_chunk falls back to this when
// a position's time_to_expiry is at or before the simulation horizon --
// black_scholes_price requires time_to_expiry > 0 and is not defined at
// T == 0 (division by sqrt(T)).
inline double intrinsic_value(OptionType type, double spot, double strike) {
  return type == OptionType::kCall ? std::max(spot - strike, 0.0) : std::max(strike - spot, 0.0);
}
}  // namespace detail

// ---------------------------------------------------------------------------
// Simulate scenarios [lo, hi) with a single RNG stream seeded by `seed`,
// writing each scenario's FIRM-level loss to firm_losses[lo..hi) and, if
// desk_losses is non-null, each scenario's per-desk loss to
// (*desk_losses)[d][lo..hi) for every desk d (desk_losses must already be
// sized [portfolio.num_desks][>= hi] by the caller).
//
// Writes are confined to the half-open range [lo, hi) of caller-owned,
// preallocated arrays. Two calls over disjoint ranges touch disjoint memory,
// so they need no synchronization between them -- this is what lets P2-3 run
// many of these concurrently on one pool with no new atomics.
// ---------------------------------------------------------------------------
inline void simulate_chunk(const Portfolio& portfolio, const PositionBaseValues& base,
                           const ScenarioParams& params, std::int64_t lo, std::int64_t hi,
                           std::uint64_t seed, double* firm_losses,
                           std::vector<std::vector<double>>* desk_losses = nullptr) {
  const std::size_t m = portfolio.underlyings.size();
  const double h = params.horizon_years;
  const double sqrt_h = std::sqrt(h);

  RngStream rng(seed);
  std::vector<double> z(m), eps(m), spot_h(m);
  std::vector<double> desk_pnl(portfolio.num_desks, 0.0);

  for (std::int64_t s = lo; s < hi; ++s) {
    for (std::size_t i = 0; i < m; ++i) z[i] = rng.next_standard_normal();
    // eps = chol * z. chol is lower-triangular (chol(i,k) == 0 for k > i), so
    // summing k in [0, i] alone is exact, not a truncation.
    for (std::size_t i = 0; i < m; ++i) {
      double acc = 0.0;
      for (std::size_t k = 0; k <= i; ++k) acc += portfolio.chol(i, k) * z[k];
      eps[i] = acc;
    }
    for (std::size_t i = 0; i < m; ++i) {
      const Underlying& u = portfolio.underlyings[i];
      spot_h[i] =
          u.spot * std::exp((params.drift - 0.5 * u.vol * u.vol) * h + u.vol * sqrt_h * eps[i]);
    }

    std::fill(desk_pnl.begin(), desk_pnl.end(), 0.0);
    for (std::size_t i = 0; i < portfolio.stocks.size(); ++i) {
      const StockPosition& pos = portfolio.stocks[i];
      const double new_value = pos.quantity * spot_h[pos.underlying];
      desk_pnl[pos.desk] += new_value - base.stocks[i];
    }
    for (std::size_t i = 0; i < portfolio.options.size(); ++i) {
      const VanillaPosition& pos = portfolio.options[i];
      const Underlying& u = portfolio.underlyings[pos.underlying];
      const double remaining_t = pos.time_to_expiry - h;
      double new_value;
      if (remaining_t > 0.0) {
        const BlackScholesInputs in{spot_h[pos.underlying], pos.strike, portfolio.rate, u.vol,
                                    remaining_t};
        new_value = pos.quantity * black_scholes_price(pos.type, in);
      } else {
        new_value =
            pos.quantity * detail::intrinsic_value(pos.type, spot_h[pos.underlying], pos.strike);
      }
      desk_pnl[pos.desk] += new_value - base.options[i];
    }

    double firm_pnl = 0.0;
    for (std::size_t d = 0; d < portfolio.num_desks; ++d) firm_pnl += desk_pnl[d];
    firm_losses[s] = -firm_pnl;
    if (desk_losses != nullptr) {
      for (std::size_t d = 0; d < portfolio.num_desks; ++d) {
        (*desk_losses)[d][static_cast<std::size_t>(s)] = -desk_pnl[d];
      }
    }
  }
}

// Serial reference: the entire [0, n_scenarios) range as one chunk (chunk
// index 0), one thread, no pool. See the file header for why this is not
// expected to bit-match P2-3's parallel engine.
inline std::vector<double> run_serial(const Portfolio& portfolio, const ScenarioParams& params) {
  const PositionBaseValues base = compute_base_values(portfolio);
  std::vector<double> losses(static_cast<std::size_t>(params.n_scenarios));
  simulate_chunk(portfolio, base, params, 0, params.n_scenarios, chunk_seed(params.seed, 0),
                params.n_scenarios > 0 ? losses.data() : nullptr);
  return losses;
}

// ---------------------------------------------------------------------------
// Empirical VaR/ES from a set of simulated losses.
//
// Quantile estimator: sort losses ascending; let k = ceil((1 - alpha) * N)
// (clamped to [1, N]). VaR_alpha is the smallest of the worst k losses, i.e.
// losses[N - k] in 0-indexed ascending order (the nearest-rank / nearest-rank
// empirical quantile, no interpolation between order statistics). ES_alpha is
// the mean of those same worst k losses. This choice is exact and
// reproducible (pure sort + integer indexing -- no interpolation-related
// floating-point ambiguity) and guarantees ES >= VaR for any N with k >= 1,
// since the mean of a set is never less than its minimum element.
// ---------------------------------------------------------------------------
inline QuantileResult compute_var_es(std::vector<double> losses, double alpha) {
  if (losses.empty()) throw std::invalid_argument("compute_var_es: no scenarios");
  std::sort(losses.begin(), losses.end());
  const auto n = static_cast<std::int64_t>(losses.size());
  auto k = static_cast<std::int64_t>(std::ceil((1.0 - alpha) * static_cast<double>(n)));
  if (k < 1) k = 1;
  if (k > n) k = n;

  QuantileResult r;
  r.var = losses[static_cast<std::size_t>(n - k)];
  double sum = 0.0;
  for (std::int64_t i = n - k; i < n; ++i) sum += losses[static_cast<std::size_t>(i)];
  r.es = sum / static_cast<double>(k);
  return r;
}

// Aggregate dollar-delta per underlying, across a desk's positions (or all
// positions, with desk == kAllDesks). A stock's delta is its quantity
// identically; an option's is quantity * black_scholes_greeks(...).delta.
inline std::vector<double> aggregate_delta_per_underlying(const Portfolio& portfolio,
                                                           std::size_t desk = kAllDesks) {
  std::vector<double> delta(portfolio.underlyings.size(), 0.0);
  for (const StockPosition& s : portfolio.stocks) {
    if (desk != kAllDesks && s.desk != desk) continue;
    delta[s.underlying] += s.quantity;
  }
  for (const VanillaPosition& o : portfolio.options) {
    if (desk != kAllDesks && o.desk != desk) continue;
    const Underlying& u = portfolio.underlyings[o.underlying];
    const BlackScholesInputs in{u.spot, o.strike, portfolio.rate, u.vol, o.time_to_expiry};
    delta[o.underlying] += o.quantity * black_scholes_greeks(o.type, in).delta;
  }
  return delta;
}

// ---------------------------------------------------------------------------
// Delta-normal VaR/ES: approximate horizon P&L as linear in each
// underlying's (correlated) Gaussian shock -- keep delta, drop gamma, vega,
// and every other higher-order term. A linear combination of jointly-normal
// variables is itself normal, so VaR/ES have closed forms and need no
// simulation. This is precisely the approximation full revaluation
// (simulate_chunk + compute_var_es) exists to go beyond; see
// test_risk_engine.cpp's ShortGammaBook... test for where the two diverge.
//
// Mean P&L is treated as zero: this matches the project's drift=0 default,
// and delta-normal VaR is conventionally a volatility-only risk number
// ("how much could this move"), not a P&L forecast. A nonzero real-world
// drift is not folded into this closed form.
// ---------------------------------------------------------------------------
inline QuantileResult delta_normal_var_es(const Portfolio& portfolio, double horizon_years,
                                          double alpha, std::size_t desk = kAllDesks) {
  const std::vector<double> delta = aggregate_delta_per_underlying(portfolio, desk);
  const std::size_t m = portfolio.underlyings.size();
  const double sqrt_h = std::sqrt(horizon_years);

  // w_i: portfolio P&L's sensitivity to the i-th correlated standard-normal
  // shock eps_i, the same eps simulate_chunk draws (dollar delta * sigma_i *
  // sqrt(h), from the same dS_i ~ S_i*sigma_i*sqrt(h)*eps_i first-order
  // approximation simulate_chunk's exact GBM formula reduces to for small
  // moves).
  std::vector<double> w(m);
  for (std::size_t i = 0; i < m; ++i) {
    const Underlying& u = portfolio.underlyings[i];
    w[i] = delta[i] * u.spot * u.vol * sqrt_h;
  }

  // Var(P&L) = w^T * Corr * w: Cov(eps) == the correlation matrix exactly
  // (eps = chol * z, chol * chol^T == Corr, z iid standard normal).
  double variance = 0.0;
  for (std::size_t i = 0; i < m; ++i) {
    for (std::size_t j = 0; j < m; ++j) {
      variance += w[i] * portfolio.correlation(i, j) * w[j];
    }
  }
  const double sigma_pnl = std::sqrt(variance);

  const double z = inverse_norm_cdf(alpha);
  QuantileResult r;
  r.var = z * sigma_pnl;
  // Closed-form Expected Shortfall of a N(0, sigma^2) loss: ES_alpha =
  // sigma * phi(z_alpha) / (1 - alpha).
  r.es = sigma_pnl * norm_pdf(z) / (1.0 - alpha);
  return r;
}

}  // namespace taskforge::risk
