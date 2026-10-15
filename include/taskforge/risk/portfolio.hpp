// portfolio.hpp -- underlyings, correlation, positions, and the hand-written
// Cholesky decomposition the scenario engine uses to turn independent normal
// draws into correlated ones.
// =============================================================================
// No Eigen, no Boost (Phase 2 rule): matrices here are at most ~50x50 (the
// spec's "up to about 50" underlyings), a few thousand doubles, so a minimal
// flat dense Matrix is all this needs -- no sparse representation, no BLAS.
//
// Portfolio is an immutable, plain-data snapshot once built: nothing in it is
// mutated during a simulation run. That is deliberate, not incidental --
// Phase 2 avoids new atomics, and the parallel engine (P2-3) has many chunk
// tasks reading the same Portfolio concurrently. A const-only read of
// never-mutated data needs no synchronization at all; giving Portfolio any
// lazily-computed mutable cache would turn "read portfolio.foo()" into a new
// data race to reason about for no benefit, so precomputed per-run values
// (like each position's value today) are plain local vectors the caller
// builds once and threads through explicitly -- see engine.hpp.
// =============================================================================

#pragma once

#include "taskforge/risk/black_scholes.hpp"
#include "taskforge/risk/rng.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace taskforge::risk {

// ---------------------------------------------------------------------------
// Minimal flat dense matrix. Row-major, (i, j) access, nothing fancier.
// ---------------------------------------------------------------------------
class Matrix {
 public:
  Matrix() : rows_(0), cols_(0) {}  // empty 0x0; lets Portfolio default-construct
  Matrix(std::size_t rows, std::size_t cols, double fill = 0.0)
      : rows_(rows), cols_(cols), data_(rows * cols, fill) {}

  double& operator()(std::size_t i, std::size_t j) { return data_[i * cols_ + j]; }
  double operator()(std::size_t i, std::size_t j) const { return data_[i * cols_ + j]; }

  std::size_t rows() const { return rows_; }
  std::size_t cols() const { return cols_; }

 private:
  std::size_t rows_, cols_;
  std::vector<double> data_;
};

// ---------------------------------------------------------------------------
// Cholesky decomposition: A = L * L^T, L lower-triangular. Hand-written
// (Cholesky-Banachiewicz, the textbook row-by-row form), O(n^3), fine for the
// matrix sizes here (n <= ~50).
//
// Returns false -- REJECTS -- rather than producing garbage, the first time a
// diagonal pivot would be non-positive. That is the operational definition of
// "not positive definite" this function uses: a true PD matrix never produces
// a non-positive pivot (every leading principal submatrix of a PD matrix is
// itself PD, so its Cholesky diagonal entries are real square roots of
// positive numbers); a matrix that is not PD (or only positive SEMI-definite,
// i.e. singular) will hit a zero or negative pivot at or before the first
// point of rank deficiency. On success, `out` is resized to A's dimensions and
// holds L; on failure, `out`'s contents are unspecified -- callers must check
// the return value before using it.
// ---------------------------------------------------------------------------
inline bool cholesky(const Matrix& a, Matrix& out) {
  if (a.rows() != a.cols()) {
    throw std::invalid_argument("cholesky: matrix must be square");
  }
  const std::size_t n = a.rows();
  out = Matrix(n, n, 0.0);

  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j <= i; ++j) {
      double sum = a(i, j);
      for (std::size_t k = 0; k < j; ++k) {
        sum -= out(i, k) * out(j, k);
      }
      if (i == j) {
        if (sum <= 0.0) return false;  // not positive definite: reject
        out(i, i) = std::sqrt(sum);
      } else {
        out(i, j) = sum / out(j, j);
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Deterministically generate an M x M correlation matrix from a seed.
//
// Draws a random M x k factor-loading matrix B (k = min(M, 4) factors) from
// chunk_seed(seed, 0)'s stream, forms the Gram matrix B*B^T + eps*I, and
// normalizes it to unit diagonal. B*B^T is positive SEMI-definite by
// construction (it is a Gram matrix: x^T(BB^T)x = |B^T x|^2 >= 0 for every
// x), and adding eps*I (eps > 0) strictly lifts every eigenvalue by eps,
// making the result positive DEFINITE -- so this generator's output always
// passes cholesky() above by construction, which is what makes portfolio
// construction deterministic and non-rejecting in practice. cholesky()'s
// REJECT path is still real and still load-bearing: it is a generic guard
// against bad input from anywhere, exercised directly in
// tests/test_risk_portfolio.cpp with a hand-built non-PD matrix.
// ---------------------------------------------------------------------------
inline Matrix generate_correlation_matrix(std::uint64_t seed, std::size_t m) {
  const std::size_t k = m < 4 ? m : 4;
  Matrix b(m, k);
  RngStream rng(chunk_seed(seed, 0));
  for (std::size_t i = 0; i < m; ++i) {
    for (std::size_t j = 0; j < k; ++j) b(i, j) = rng.next_standard_normal();
  }

  constexpr double kEps = 1e-3;  // strictly lifts every eigenvalue above 0
  Matrix gram(m, m);
  for (std::size_t i = 0; i < m; ++i) {
    for (std::size_t j = 0; j < m; ++j) {
      double sum = (i == j) ? kEps : 0.0;
      for (std::size_t f = 0; f < k; ++f) sum += b(i, f) * b(j, f);
      gram(i, j) = sum;
    }
  }

  Matrix corr(m, m);
  for (std::size_t i = 0; i < m; ++i) {
    for (std::size_t j = 0; j < m; ++j) {
      corr(i, j) = gram(i, j) / std::sqrt(gram(i, i) * gram(j, j));
    }
  }
  // Diagonal is exactly 1 up to floating-point rounding of the sqrt above;
  // pin it exactly so downstream code can rely on corr(i,i) == 1.0.
  for (std::size_t i = 0; i < m; ++i) corr(i, i) = 1.0;
  return corr;
}

// ---------------------------------------------------------------------------
// Instruments and the portfolio that holds them.
// ---------------------------------------------------------------------------
struct Underlying {
  std::string name;
  double spot;  // S_i(0), today's price
  double vol;   // sigma_i, annualized
};

// A plain shareholding: no optionality, delta == quantity identically. Useful
// on its own (desks hold stock, not just options) and as the simplest
// possible instrument to validate the simulation machinery against a clean
// analytic reference (see test_risk_portfolio.cpp's "stock-only portfolio").
struct StockPosition {
  std::size_t underlying;  // index into Portfolio::underlyings
  double quantity;         // signed: positive long, negative short
  std::size_t desk = 0;    // which desk this position belongs to
};

struct VanillaPosition {
  std::size_t underlying;
  OptionType type;
  double strike;
  double time_to_expiry;  // years, as of today (t=0)
  double quantity;        // signed
  std::size_t desk = 0;
};

// Immutable once built. rate is a single firm-wide continuously-compounded
// risk-free rate (no multi-curve, no per-underlying funding -- the same
// "constant, simple, and said so" spirit as constant vol and no dividends).
struct Portfolio {
  std::vector<Underlying> underlyings;
  Matrix correlation;  // M x M
  Matrix chol;          // M x M, lower-triangular Cholesky factor of correlation
  double rate = 0.0;
  std::vector<StockPosition> stocks;
  std::vector<VanillaPosition> options;
  std::size_t num_desks = 1;
};

// Build a Portfolio with a deterministically-generated correlation matrix and
// its Cholesky factor precomputed once (so every scenario reuses it rather
// than re-decomposing per draw). Throws if the generated matrix somehow fails
// cholesky() (should not happen -- see generate_correlation_matrix's comment
// -- but this is the one place a genuinely malformed portfolio would surface,
// so it is checked rather than silently producing NaNs downstream).
inline Portfolio make_portfolio(std::vector<Underlying> underlyings, double rate,
                                std::uint64_t correlation_seed) {
  Portfolio p;
  p.underlyings = std::move(underlyings);
  p.rate = rate;
  p.correlation = generate_correlation_matrix(correlation_seed, p.underlyings.size());
  if (!cholesky(p.correlation, p.chol)) {
    throw std::runtime_error("make_portfolio: generated correlation matrix was not PD "
                             "(should be unreachable -- see generate_correlation_matrix)");
  }
  return p;
}

}  // namespace taskforge::risk
