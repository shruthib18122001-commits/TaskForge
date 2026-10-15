// Phase 2 / P2-4 -- the DAG version of the risk engine: chunk nodes simulate
// and revalue every desk's positions; per-desk and firm-level quantile nodes
// depend on all of them; a report node depends on those. Two things worth
// proving here beyond "it runs": the DAG's result must equal running the
// same chunk plan outside of any graph at all (the graph is a scheduling
// device, not a second source of truth for the math), and firm-level ES must
// be subadditive across desks -- firm ES <= sum of desk ES, for the EXACT
// same scenario set and the same tail size k. See engine.hpp's run_dag
// comment for why the analogous statement for VaR is NOT true in general.

#include "taskforge/risk/engine.hpp"
#include "taskforge/risk/portfolio.hpp"
#include "taskforge/thread_pool.hpp"

#include <gtest/gtest.h>

using namespace taskforge;
using namespace taskforge::risk;

namespace {

// Three desks, three underlyings, a mix of stock and option positions so
// each desk's P&L has a genuinely different shape (not just scaled copies of
// each other) -- that is what makes the subadditivity test meaningful rather
// than a degenerate equality.
Portfolio make_multi_desk_portfolio() {
  const std::vector<Underlying> underlyings = {
      {"A", 100.0, 0.22},
      {"B", 70.0, 0.30},
      {"C", 130.0, 0.18},
  };
  Portfolio p = make_portfolio(underlyings, 0.02, 4242);
  p.num_desks = 3;
  p.stocks = {
      {0, 300.0, 0},   // desk 0: long stock A
      {1, -150.0, 1},  // desk 1: short stock B
  };
  p.options = {
      {1, OptionType::kCall, 75.0, 0.4, -400.0, 1},  // desk 1: short calls on B
      {2, OptionType::kPut, 125.0, 0.3, 250.0, 2},   // desk 2: long puts on C
      {0, OptionType::kCall, 105.0, 0.25, 150.0, 2},  // desk 2: long calls on A too
  };
  return p;
}

ScenarioParams make_dag_params() {
  ScenarioParams params;
  params.horizon_years = 10.0 / 365.0;
  params.n_scenarios = 60000;
  params.seed = 86420;
  params.chunk_size = 913;  // not a divisor of n_scenarios, same as P2-3's test
  return params;
}

// Reference computation that never touches Graph at all: run the exact same
// chunk plan in a plain serial loop, writing into the same shape of arrays
// run_dag uses internally, then take quantiles the same way. If this and
// run_dag ever disagree, the DAG's scheduling (not the finance) is the bug.
DagRiskReport compute_flat_reference(const Portfolio& portfolio, const ScenarioParams& params,
                                     const std::vector<double>& alphas) {
  const PositionBaseValues base = compute_base_values(portfolio);
  const auto n = static_cast<std::size_t>(params.n_scenarios);
  std::vector<double> firm_losses(n);
  std::vector<std::vector<double>> desk_losses(portfolio.num_desks, std::vector<double>(n));

  for (const ChunkPlan& c : plan_chunks(params)) {
    simulate_chunk(portfolio, base, params, c.lo, c.hi, c.seed, firm_losses.data(), &desk_losses);
  }

  DagRiskReport r;
  r.firm.resize(alphas.size());
  for (std::size_t i = 0; i < alphas.size(); ++i) r.firm[i] = compute_var_es(firm_losses, alphas[i]);
  r.desks.resize(portfolio.num_desks);
  for (std::size_t d = 0; d < portfolio.num_desks; ++d) {
    r.desks[d].resize(alphas.size());
    for (std::size_t i = 0; i < alphas.size(); ++i) {
      r.desks[d][i] = compute_var_es(desk_losses[d], alphas[i]);
    }
  }
  return r;
}

}  // namespace

TEST(RiskEngineDag, DagResultEqualsFlatReference) {
  const Portfolio portfolio = make_multi_desk_portfolio();
  const ScenarioParams params = make_dag_params();
  const std::vector<double> alphas = {0.95, 0.99};

  const DagRiskReport flat = compute_flat_reference(portfolio, params, alphas);

  ThreadPool pool(4);
  const DagRiskReport dag = run_dag(pool, portfolio, params, alphas);

  ASSERT_EQ(dag.firm.size(), flat.firm.size());
  for (std::size_t i = 0; i < alphas.size(); ++i) {
    EXPECT_EQ(dag.firm[i].var, flat.firm[i].var) << "alpha=" << alphas[i];
    EXPECT_EQ(dag.firm[i].es, flat.firm[i].es) << "alpha=" << alphas[i];
  }
  ASSERT_EQ(dag.desks.size(), flat.desks.size());
  for (std::size_t d = 0; d < dag.desks.size(); ++d) {
    ASSERT_EQ(dag.desks[d].size(), flat.desks[d].size());
    for (std::size_t i = 0; i < alphas.size(); ++i) {
      EXPECT_EQ(dag.desks[d][i].var, flat.desks[d][i].var) << "desk=" << d << " alpha=" << alphas[i];
      EXPECT_EQ(dag.desks[d][i].es, flat.desks[d][i].es) << "desk=" << d << " alpha=" << alphas[i];
    }
  }
}

TEST(RiskEngineDag, DagResultDeterministicAcrossThreadCounts) {
  // Same claim P2-3 makes for run_parallel, now for run_dag: the graph's
  // scheduling (how many workers, which one picks up which chunk/quantile
  // node) must not change the answer, because every chunk still writes to
  // its own disjoint slice and every quantile node still waits for ALL
  // chunk nodes via the DAG's dependency edges before reading them.
  const Portfolio portfolio = make_multi_desk_portfolio();
  const ScenarioParams params = make_dag_params();
  const std::vector<double> alphas = {0.95, 0.99};

  DagRiskReport reference;
  bool have_reference = false;
  for (unsigned threads : {1u, 2u, 4u, 8u}) {
    ThreadPool pool(threads);
    DagRiskReport r = run_dag(pool, portfolio, params, alphas);
    if (!have_reference) {
      reference = r;
      have_reference = true;
      continue;
    }
    for (std::size_t i = 0; i < alphas.size(); ++i) {
      EXPECT_EQ(r.firm[i].var, reference.firm[i].var) << "threads=" << threads;
      EXPECT_EQ(r.firm[i].es, reference.firm[i].es) << "threads=" << threads;
    }
  }
}

TEST(RiskEngineDag, FirmEsIsSubadditiveAcrossDesks) {
  // Why this holds EXACTLY (not just "usually", and not a property of this
  // particular portfolio): let k be the shared tail size (same alpha, same
  // N, so the same k for the firm and for every desk). Let W be the SET of
  // the firm's own worst-k scenario indices. Firm ES is, by definition, the
  // average of (sum over desks of that desk's loss) over W, which splits
  // into a sum over desks of "desk d's average loss over W". For each desk
  // d, averaging its losses over SOME size-k set W can be at most as large
  // as averaging over desk d's OWN worst-k set -- its own worst-k set is, by
  // construction, the size-k subset that MAXIMIZES that average. So firm ES
  // = sum_d (desk d's average over W) <= sum_d (desk d's own ES). This is an
  // argmax-over-subsets inequality, true for any portfolio and any scenario
  // set -- not something that could fail to replicate on a different seed.
  const Portfolio portfolio = make_multi_desk_portfolio();
  const ScenarioParams params = make_dag_params();
  const std::vector<double> alphas = {0.90, 0.95, 0.99};

  ThreadPool pool(4);
  const DagRiskReport r = run_dag(pool, portfolio, params, alphas);

  for (std::size_t i = 0; i < alphas.size(); ++i) {
    double sum_desk_es = 0.0;
    for (const auto& desk : r.desks) sum_desk_es += desk[i].es;
    EXPECT_LE(r.firm[i].es, sum_desk_es + 1e-9)
        << "alpha=" << alphas[i] << " firm.es=" << r.firm[i].es
        << " sum_desk_es=" << sum_desk_es;
  }
}

TEST(RiskEngineDag, FirmVarIsNotEqualToSumOfDeskVars) {
  // The flip side of the subadditivity test above: VaR has no averaging
  // argument to fall back on (see engine.hpp's run_dag comment), so there is
  // no reason to expect firm VaR to equal, exceed, or fall short of the sum
  // of desk VaRs in general. This test only documents that they are, in
  // fact, different numbers for a realistic multi-desk book -- i.e. that
  // "just add up the desks' VaRs" is not a shortcut this engine happens to
  // validate, which is the whole reason the firm gets its own quantile node
  // instead of being computed that way.
  const Portfolio portfolio = make_multi_desk_portfolio();
  const ScenarioParams params = make_dag_params();
  const std::vector<double> alphas = {0.95};

  ThreadPool pool(4);
  const DagRiskReport r = run_dag(pool, portfolio, params, alphas);

  double sum_desk_var = 0.0;
  for (const auto& desk : r.desks) sum_desk_var += desk[0].var;
  EXPECT_NE(r.firm[0].var, sum_desk_var)
      << "firm.var=" << r.firm[0].var << " sum_desk_var=" << sum_desk_var;
}
