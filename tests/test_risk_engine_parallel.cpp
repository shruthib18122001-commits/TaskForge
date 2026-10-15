// Phase 2 / P2-3 -- the parallel engine: run_parallel<Pool> must give
// bit-identical results regardless of which pool runs it or how many
// threads it has, because plan_chunks() (engine.hpp) fixes the (range, seed)
// pairs before any task is ever submitted. This file proves that claim
// directly, across all three pools and a spread of thread counts, and
// separately checks the parallel path reproduces the same financial sanity
// properties P2-2 already proved for the serial path.

#include "taskforge/async_pool.hpp"
#include "taskforge/mutex_queue_pool.hpp"
#include "taskforge/risk/engine.hpp"
#include "taskforge/risk/portfolio.hpp"
#include "taskforge/thread_pool.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <exception>
#include <stdexcept>
#include <vector>

using namespace taskforge;
using namespace taskforge::risk;

namespace {

Portfolio make_test_portfolio() {
  const std::vector<Underlying> underlyings = {
      {"A", 100.0, 0.2},
      {"B", 60.0, 0.35},
      {"C", 150.0, 0.25},
  };
  Portfolio p = make_portfolio(underlyings, 0.02, 2026);
  p.stocks = {{0, 100.0, 0}, {1, -50.0, 0}};
  p.options = {
      {1, OptionType::kCall, 65.0, 0.5, 200.0, 0},
      {2, OptionType::kPut, 140.0, 0.25, -80.0, 0},
  };
  return p;
}

ScenarioParams make_test_params() {
  ScenarioParams params;
  params.horizon_years = 10.0 / 365.0;
  params.n_scenarios = 40000;
  params.seed = 13579;
  params.chunk_size = 777;  // deliberately not a divisor of n_scenarios
  return params;
}

// A minimal stand-in satisfying run_parallel<Pool>'s contract (submit/
// wait_idle/first_exception), used only to check run_parallel's own
// rethrow-after-wait_idle behavior in isolation from any real pool's
// scheduling. Declared at namespace scope because a local class cannot have
// a member function template (submit<F>) in C++17.
struct ThrowingPool {
  explicit ThrowingPool(unsigned) {}
  template <class F>
  void submit(F&&) {
    try {
      throw std::runtime_error("boom");
    } catch (...) {
      exc_ = std::current_exception();
    }
  }
  void wait_idle() {}
  std::exception_ptr first_exception() const { return exc_; }
  std::exception_ptr exc_;
};

}  // namespace

TEST(RiskEngineParallel, DeterministicAcrossThreadCountsAndPools) {
  const Portfolio portfolio = make_test_portfolio();
  const ScenarioParams params = make_test_params();

  std::vector<double> reference;
  bool have_reference = false;

  for (unsigned threads : {1u, 2u, 4u, 8u}) {
    {
      ThreadPool pool(threads);
      std::vector<double> losses = run_parallel(pool, portfolio, params);
      if (!have_reference) {
        reference = losses;
        have_reference = true;
      } else {
        ASSERT_EQ(losses, reference) << "ThreadPool threads=" << threads;
      }
    }
    {
      MutexQueuePool pool(threads);
      std::vector<double> losses = run_parallel(pool, portfolio, params);
      ASSERT_EQ(losses, reference) << "MutexQueuePool threads=" << threads;
    }
    {
      AsyncPool pool(threads);
      std::vector<double> losses = run_parallel(pool, portfolio, params);
      ASSERT_EQ(losses, reference) << "AsyncPool threads=" << threads;
    }
  }
}

TEST(RiskEngineParallel, MatchesSerialEngineStatistically) {
  // run_parallel and run_serial are NOT expected to be bit-identical (see
  // engine.hpp's file header) -- different chunk boundaries draw different
  // random numbers. But both simulate the SAME model, so their VaR estimates
  // should agree within the combined statistical noise of two independent
  // Monte Carlo runs of the same size.
  const Portfolio portfolio = make_test_portfolio();
  ScenarioParams params = make_test_params();
  params.n_scenarios = 200000;
  const double alpha = 0.95;

  const std::vector<double> serial_losses = run_serial(portfolio, params);
  ThreadPool pool(4);
  const std::vector<double> parallel_losses = run_parallel(pool, portfolio, params);

  const QuantileResult serial_r = compute_var_es(serial_losses, alpha);
  const QuantileResult parallel_r = compute_var_es(parallel_losses, alpha);

  // Rough standard error for an empirical VaR from N draws of an
  // approximately-normal loss distribution, same form used in
  // test_risk_engine.cpp; "approximately" is good enough for a 5-SE-wide
  // sanity band comparing two independent Monte Carlo estimates of the same
  // quantity, not for a precision claim.
  const double sigma_estimate = serial_r.var / inverse_norm_cdf(alpha);
  const double se = std::sqrt(alpha * (1.0 - alpha) / static_cast<double>(params.n_scenarios)) *
                    sigma_estimate / norm_pdf(inverse_norm_cdf(alpha));
  const int kSeFactor = 6;  // two independent estimates being compared, not one vs a closed form
  EXPECT_NEAR(serial_r.var, parallel_r.var, kSeFactor * se)
      << "serial.var=" << serial_r.var << " parallel.var=" << parallel_r.var << " se=" << se;
}

TEST(RiskEngineParallel, PropagatesFirstExceptionFromAChunk) {
  // Exception-safety policy (Phase 1's rule, reused as-is): a task that
  // throws is caught by the pool, recorded, and surfaced via
  // first_exception() rather than crashing the pool or losing the error.
  // run_parallel's contract is to rethrow that after wait_idle(); this
  // confirms it does, using a trivial pool and params sized for a single
  // chunk so there is no ambiguity about which chunk "the" exception came
  // from.
  const Portfolio portfolio = make_test_portfolio();
  ScenarioParams params = make_test_params();
  params.n_scenarios = 10;
  ThrowingPool pool(1);
  EXPECT_THROW(run_parallel(pool, portfolio, params), std::runtime_error);
}
