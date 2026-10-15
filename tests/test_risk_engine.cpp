// Phase 2 / P2-2 -- serial full-revaluation VaR/ES engine, delta-normal VaR/ES,
// and the comparison between them.
//
// Per the spec: a stock-only portfolio's Monte Carlo VaR matches the
// analytic (delta-normal) normal VaR within standard error; ES >= VaR; VaR
// rises with alpha; same seed gives the same answer. Plus two things the
// spec explicitly calls out as the point of doing full revaluation at all:
// where delta-normal diverges badly (a short-gamma option book), and
// confirmation that the correlation structure is actually being used (not
// silently treated as independent).

#include "taskforge/risk/engine.hpp"
#include "taskforge/risk/portfolio.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>

using namespace taskforge::risk;

namespace {

// Asymptotic standard error of an empirical alpha-quantile estimator from N
// iid draws of a distribution with density f at the quantile:
//   SE(Q_hat_alpha) ~= sqrt(alpha*(1-alpha)/N) / f(Q_alpha).
// For a loss distribution that is (to excellent approximation, at the small
// horizons used below) N(0, sigma^2), f(Q_alpha) = norm_pdf(z_alpha)/sigma,
// giving the form used here. This is the standard theoretical basis for
// "matches within standard error" claims about simulated quantiles -- not an
// arbitrary percentage.
double empirical_var_standard_error(double alpha, double sigma_normal_approx, std::int64_t n) {
  const double z = inverse_norm_cdf(alpha);
  return std::sqrt(alpha * (1.0 - alpha) / static_cast<double>(n)) * sigma_normal_approx /
         norm_pdf(z);
}

}  // namespace

TEST(RiskEngine, StockOnlyMonteCarloVarMatchesAnalyticNormalVarWithinStandardError) {
  // A single stock position (no optionality at all) at a 1-day horizon: the
  // log-return over one day is small enough that the true (lognormal)
  // distribution of P&L is extremely close to the linear/Gaussian
  // approximation delta-normal VaR assumes -- the two should then agree
  // within the Monte Carlo estimator's own statistical noise.
  const std::vector<Underlying> underlyings = {{"A", 100.0, 0.2}};
  Portfolio p = make_portfolio(underlyings, 0.03, 1);
  p.stocks = {{0, 1000.0, 0}};

  ScenarioParams params;
  params.horizon_years = 1.0 / 365.0;
  params.drift = 0.0;
  params.n_scenarios = 500000;
  params.seed = 42;

  const double alpha = 0.95;
  const std::vector<double> losses = run_serial(p, params);
  const QuantileResult full_reval = compute_var_es(losses, alpha);
  const QuantileResult analytic = delta_normal_var_es(p, params.horizon_years, alpha);

  const int kSeFactor = 5;
  const double se = empirical_var_standard_error(alpha, analytic.var / inverse_norm_cdf(alpha),
                                                 params.n_scenarios);
  EXPECT_NEAR(full_reval.var, analytic.var, kSeFactor * se)
      << "full_reval.var=" << full_reval.var << " analytic.var=" << analytic.var << " se=" << se;
}

TEST(RiskEngine, EsAtLeastVar) {
  const std::vector<Underlying> underlyings = {{"A", 100.0, 0.25}, {"B", 60.0, 0.4}};
  Portfolio p = make_portfolio(underlyings, 0.02, 7);
  p.stocks = {{0, 100.0, 0}};
  p.options = {{1, OptionType::kCall, 60.0, 0.5, -50.0, 0}};  // a short call too

  ScenarioParams params;
  params.horizon_years = 10.0 / 365.0;
  params.n_scenarios = 50000;
  params.seed = 5;
  const std::vector<double> losses = run_serial(p, params);

  for (double alpha : {0.90, 0.95, 0.99}) {
    const QuantileResult r = compute_var_es(losses, alpha);
    EXPECT_GE(r.es, r.var - 1e-9) << "alpha=" << alpha;
  }
  // The closed-form delta-normal version must satisfy the same structural
  // inequality (it is a property of the normal ES/VaR formulas, not of the
  // simulation).
  for (double alpha : {0.90, 0.95, 0.99}) {
    const QuantileResult r = delta_normal_var_es(p, params.horizon_years, alpha);
    EXPECT_GE(r.es, r.var - 1e-9) << "delta-normal alpha=" << alpha;
  }
}

TEST(RiskEngine, VarRisesWithAlpha) {
  const std::vector<Underlying> underlyings = {{"A", 80.0, 0.3}};
  Portfolio p = make_portfolio(underlyings, 0.01, 9);
  p.stocks = {{0, 500.0, 0}};

  ScenarioParams params;
  params.horizon_years = 10.0 / 365.0;
  params.n_scenarios = 50000;
  params.seed = 11;
  const std::vector<double> losses = run_serial(p, params);

  const double var90 = compute_var_es(losses, 0.90).var;
  const double var95 = compute_var_es(losses, 0.95).var;
  const double var99 = compute_var_es(losses, 0.99).var;
  EXPECT_LT(var90, var95);
  EXPECT_LT(var95, var99);
}

TEST(RiskEngine, SameSeedGivesSameAnswer) {
  const std::vector<Underlying> underlyings = {{"A", 100.0, 0.2}, {"B", 50.0, 0.35}};
  Portfolio p = make_portfolio(underlyings, 0.02, 3);
  p.stocks = {{0, 10.0, 0}};
  p.options = {{1, OptionType::kPut, 55.0, 0.75, 20.0, 0}};

  ScenarioParams params;
  params.horizon_years = 1.0 / 365.0;
  params.n_scenarios = 20000;
  params.seed = 123456789;

  const std::vector<double> a = run_serial(p, params);
  const std::vector<double> b = run_serial(p, params);
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i], b[i]) << "scenario " << i;
  }
}

TEST(RiskEngine, ShortGammaStraddleDivergesSharplyFromDeltaNormal) {
  // The point of full revaluation: a short, at-the-money straddle is close to
  // delta-NEUTRAL (long gamma/vega risk roughly cancels out of the linear
  // delta sum), so delta-normal reports a small VaR for it. But the position
  // is short gamma and short vega -- it loses money on a big move in EITHER
  // direction, and over a 10-day horizon with 30%+ vol and only ~20 days left
  // to the straddle's own expiry, gamma is large. Full revaluation (which
  // actually reprices the options, convexity and all) must show a much
  // larger VaR than the linear approximation does.
  const std::vector<Underlying> underlyings = {{"A", 100.0, 0.30}};
  Portfolio p = make_portfolio(underlyings, 0.02, 21);
  const double strike = 100.0;      // at the money
  const double expiry = 30.0 / 365.0;
  const double qty = -1000.0;       // short both legs
  p.options = {
      {0, OptionType::kCall, strike, expiry, qty, 0},
      {0, OptionType::kPut, strike, expiry, qty, 0},
  };

  ScenarioParams params;
  params.horizon_years = 10.0 / 365.0;
  params.n_scenarios = 200000;
  params.seed = 2024;
  const double alpha = 0.95;

  const std::vector<double> losses = run_serial(p, params);
  const QuantileResult full_reval = compute_var_es(losses, alpha);
  const QuantileResult analytic = delta_normal_var_es(p, params.horizon_years, alpha);

  ASSERT_GT(analytic.var, 0.0);  // sanity: delta-normal still reports SOME risk
  EXPECT_GT(full_reval.var, 2.0 * analytic.var)
      << "full_reval.var=" << full_reval.var << " analytic.var=" << analytic.var
      << " (delta-normal should badly understate a short-gamma book's risk)";
}

TEST(RiskEngine, FullRevaluationReflectsPortfolioCorrelation) {
  // Two long stock positions, same direction, on two assets whose return
  // correlation is set by hand (bypassing generate_correlation_matrix, for
  // full control). Higher correlation between same-direction positions must
  // produce a higher portfolio VaR -- if simulate_chunk's eps = chol * z step
  // were silently wrong (e.g. treating shocks as independent regardless of
  // the correlation matrix), this test would fail to show any difference.
  auto build = [](double rho) {
    Portfolio p;
    p.underlyings = {{"A", 100.0, 0.2}, {"B", 100.0, 0.2}};
    p.rate = 0.0;
    Matrix corr(2, 2);
    corr(0, 0) = 1.0;
    corr(1, 1) = 1.0;
    corr(0, 1) = rho;
    corr(1, 0) = rho;
    p.correlation = corr;
    Matrix l(0, 0);
    if (!cholesky(corr, l)) throw std::runtime_error("test matrix unexpectedly not PD");
    p.chol = l;
    p.num_desks = 1;
    p.stocks = {{0, 1000.0, 0}, {1, 1000.0, 0}};
    return p;
  };

  ScenarioParams params;
  params.horizon_years = 1.0 / 365.0;
  params.n_scenarios = 300000;
  params.seed = 99;

  const Portfolio low_corr = build(0.0);
  const Portfolio high_corr = build(0.9);
  const double var_low = compute_var_es(run_serial(low_corr, params), 0.95).var;
  const double var_high = compute_var_es(run_serial(high_corr, params), 0.95).var;

  EXPECT_GT(var_high, 1.2 * var_low) << "var_low=" << var_low << " var_high=" << var_high;
}

TEST(RiskEngine, OptionExpiringWithinHorizonFallsBackToIntrinsicValue) {
  // A position whose time_to_expiry is shorter than the VaR horizon must not
  // feed a non-positive time-to-expiry into black_scholes_price (undefined:
  // division by sqrt(0)). simulate_chunk falls back to intrinsic value in
  // that case -- this just checks the engine runs and produces finite,
  // sane numbers rather than NaN/inf.
  const std::vector<Underlying> underlyings = {{"A", 100.0, 0.25}};
  Portfolio p = make_portfolio(underlyings, 0.02, 4);
  p.options = {{0, OptionType::kCall, 100.0, 5.0 / 365.0, 100.0, 0}};  // expires in 5 days

  ScenarioParams params;
  params.horizon_years = 10.0 / 365.0;  // horizon is LONGER than time to expiry
  params.n_scenarios = 10000;
  params.seed = 6;

  const std::vector<double> losses = run_serial(p, params);
  for (double loss : losses) {
    ASSERT_TRUE(std::isfinite(loss));
  }
  const QuantileResult r = compute_var_es(losses, 0.95);
  EXPECT_TRUE(std::isfinite(r.var));
  EXPECT_TRUE(std::isfinite(r.es));
}
