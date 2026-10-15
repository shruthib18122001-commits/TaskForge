// Phase 2 / P2-2 -- Matrix, Cholesky, and deterministic correlation-matrix
// generation.

#include "taskforge/risk/portfolio.hpp"

#include <gtest/gtest.h>

#include <cmath>

using taskforge::risk::cholesky;
using taskforge::risk::generate_correlation_matrix;
using taskforge::risk::make_portfolio;
using taskforge::risk::Matrix;
using taskforge::risk::Underlying;

TEST(RiskPortfolio, CholeskyReconstructsOriginalMatrix) {
  // A known positive-definite 3x3 (diagonally dominant with positive
  // diagonal, a sufficient condition for PD).
  Matrix a(3, 3);
  double vals[3][3] = {{4, 2, 2}, {2, 5, 1}, {2, 1, 6}};
  for (std::size_t i = 0; i < 3; ++i)
    for (std::size_t j = 0; j < 3; ++j) a(i, j) = vals[i][j];

  Matrix l(0, 0);
  ASSERT_TRUE(cholesky(a, l));

  // Reconstruct A from L*L^T and compare.
  for (std::size_t i = 0; i < 3; ++i) {
    for (std::size_t j = 0; j < 3; ++j) {
      double sum = 0.0;
      for (std::size_t k = 0; k < 3; ++k) sum += l(i, k) * l(j, k);
      EXPECT_NEAR(sum, a(i, j), 1e-10) << "i=" << i << " j=" << j;
    }
  }
  // L is lower-triangular by construction.
  for (std::size_t i = 0; i < 3; ++i)
    for (std::size_t j = i + 1; j < 3; ++j) EXPECT_EQ(l(i, j), 0.0);
}

TEST(RiskPortfolio, CholeskyRejectsNonPositiveDefinite) {
  // [[1,2],[2,1]]: eigenvalues 3 and -1, not PD (not even a valid correlation
  // matrix -- correlation can't be 2). The second diagonal pivot works out to
  // 1 - 2^2 = -3 <= 0, so cholesky() must reject rather than take sqrt of a
  // negative number.
  Matrix a(2, 2);
  a(0, 0) = 1.0;
  a(0, 1) = 2.0;
  a(1, 0) = 2.0;
  a(1, 1) = 1.0;

  Matrix l(0, 0);
  EXPECT_FALSE(cholesky(a, l));
}

TEST(RiskPortfolio, CholeskyRejectsRankDeficientMatrix) {
  // Positive SEMI-definite but singular (rank 2, not 3): row 2 == row 1. A
  // strictly-PD requirement must reject this too, not just outright-negative
  // matrices.
  Matrix a(3, 3);
  double vals[3][3] = {{1, 1, 0}, {1, 1, 0}, {0, 0, 1}};
  for (std::size_t i = 0; i < 3; ++i)
    for (std::size_t j = 0; j < 3; ++j) a(i, j) = vals[i][j];

  Matrix l(0, 0);
  EXPECT_FALSE(cholesky(a, l));
}

TEST(RiskPortfolio, GeneratedCorrelationMatrixIsValid) {
  for (std::size_t m : {std::size_t{1}, std::size_t{2}, std::size_t{5}, std::size_t{20}}) {
    Matrix corr = generate_correlation_matrix(42, m);
    ASSERT_EQ(corr.rows(), m);
    ASSERT_EQ(corr.cols(), m);

    for (std::size_t i = 0; i < m; ++i) {
      EXPECT_NEAR(corr(i, i), 1.0, 1e-12) << "diagonal at " << i;
      for (std::size_t j = 0; j < m; ++j) {
        EXPECT_NEAR(corr(i, j), corr(j, i), 1e-12) << "symmetry at (" << i << "," << j << ")";
        EXPECT_LE(corr(i, j), 1.0 + 1e-9) << "(" << i << "," << j << ")";
        EXPECT_GE(corr(i, j), -1.0 - 1e-9) << "(" << i << "," << j << ")";
      }
    }

    // By construction (Gram matrix + eps*I, see generate_correlation_matrix's
    // comment) this must always pass cholesky -- it is never the rejection
    // path in practice, only the generic guard tested directly above.
    Matrix l(0, 0);
    EXPECT_TRUE(cholesky(corr, l)) << "m=" << m;
  }
}

TEST(RiskPortfolio, CorrelationGenerationIsDeterministic) {
  Matrix a = generate_correlation_matrix(777, 8);
  Matrix b = generate_correlation_matrix(777, 8);
  for (std::size_t i = 0; i < 8; ++i) {
    for (std::size_t j = 0; j < 8; ++j) EXPECT_EQ(a(i, j), b(i, j));
  }
}

TEST(RiskPortfolio, DifferentSeedsGiveDifferentCorrelationMatrices) {
  Matrix a = generate_correlation_matrix(1, 5);
  Matrix b = generate_correlation_matrix(2, 5);
  bool any_different = false;
  for (std::size_t i = 0; i < 5; ++i) {
    for (std::size_t j = 0; j < 5; ++j) {
      if (i != j && std::abs(a(i, j) - b(i, j)) > 1e-9) any_different = true;
    }
  }
  EXPECT_TRUE(any_different);
}

TEST(RiskPortfolio, MakePortfolioPrecomputesValidCholeskyFactor) {
  std::vector<Underlying> u = {
      {"A", 100.0, 0.2},
      {"B", 50.0, 0.3},
      {"C", 200.0, 0.15},
  };
  auto p = make_portfolio(u, 0.03, 123);
  ASSERT_EQ(p.chol.rows(), 3u);
  for (std::size_t i = 0; i < 3; ++i) {
    for (std::size_t j = 0; j < 3; ++j) {
      double sum = 0.0;
      for (std::size_t k = 0; k < 3; ++k) sum += p.chol(i, k) * p.chol(j, k);
      EXPECT_NEAR(sum, p.correlation(i, j), 1e-9) << "i=" << i << " j=" << j;
    }
  }
}
