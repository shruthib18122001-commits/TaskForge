// Phase 2 / P2-1 -- the RNG: moments, a tail check, and the determinism
// properties the whole chunking scheme depends on.
//
// Every tolerance here is derived from the relevant standard error and
// multiplied by a stated number of standard errors (SEs), per the
// no-flaky-tests rule -- never an arbitrary "within 1%".

#include "taskforge/risk/rng.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

using taskforge::risk::chunk_seed;
using taskforge::risk::RngStream;
using taskforge::risk::SplitMix64;

namespace {
constexpr int kSeToleranceFactor = 5;  // generous; this is a sanity check, not a precision test
}  // namespace

TEST(RiskRng, UniformMoments) {
  SplitMix64 rng(chunk_seed(12345, 0));
  const int n = 200000;
  double sum = 0.0, sum_sq = 0.0;
  for (int i = 0; i < n; ++i) {
    const double u = rng.next_uniform();
    ASSERT_GE(u, 0.0);
    ASSERT_LT(u, 1.0);
    sum += u;
    sum_sq += u * u;
  }
  const double mean = sum / n;
  const double var = sum_sq / n - mean * mean;

  // Uniform[0,1): true mean 0.5, true variance 1/12.
  const double mean_se = std::sqrt((1.0 / 12.0) / n);
  EXPECT_NEAR(mean, 0.5, kSeToleranceFactor * mean_se);
  // Variance of the sample variance for a bounded distribution is modest;
  // 1/12 of true variance as a loose SE proxy is generous enough not to flake
  // while still catching a badly broken generator.
  const double var_se = (1.0 / 12.0) / std::sqrt(static_cast<double>(n));
  EXPECT_NEAR(var, 1.0 / 12.0, kSeToleranceFactor * var_se);
}

TEST(RiskRng, NormalMoments) {
  RngStream rng(chunk_seed(12345, 1));
  const int n = 200000;
  double sum = 0.0, sum_sq = 0.0;
  for (int i = 0; i < n; ++i) {
    const double z = rng.next_standard_normal();
    sum += z;
    sum_sq += z * z;
  }
  const double mean = sum / n;
  const double var = sum_sq / n - mean * mean;

  // N(0,1): mean 0, variance 1. SE(mean) = sigma/sqrt(n) = 1/sqrt(n).
  // SE(sample variance) = sigma^2 * sqrt(2/n) = sqrt(2/n) for a normal.
  const double mean_se = 1.0 / std::sqrt(static_cast<double>(n));
  const double var_se = std::sqrt(2.0 / n);
  EXPECT_NEAR(mean, 0.0, kSeToleranceFactor * mean_se);
  EXPECT_NEAR(var, 1.0, kSeToleranceFactor * var_se);
}

TEST(RiskRng, NormalTailCheck) {
  // Fraction of standard-normal draws exceeding known quantiles. This is the
  // test that a broken Box-Muller (wrong formula, swapped sin/cos, degrees
  // instead of radians, ...) would actually catch even if the first two
  // moments happened to look fine.
  RngStream rng(chunk_seed(12345, 2));
  const int n = 500000;
  int above_1_645 = 0;  // P(Z > 1.645) ~= 0.05 (95th percentile)
  int above_2_326 = 0;  // P(Z > 2.326) ~= 0.01 (99th percentile)
  int below_m1_645 = 0;  // symmetry check
  for (int i = 0; i < n; ++i) {
    const double z = rng.next_standard_normal();
    if (z > 1.645) ++above_1_645;
    if (z > 2.326) ++above_2_326;
    if (z < -1.645) ++below_m1_645;
  }
  // n is a compile-time constant (const int with a literal initializer), so
  // C++17's relaxed lambda-capture rules let the body use it without a
  // capture -- clang's -Wunused-lambda-capture correctly flags [n] as dead.
  auto se = [](double p) { return std::sqrt(p * (1.0 - p) / n); };
  EXPECT_NEAR(static_cast<double>(above_1_645) / n, 0.05, kSeToleranceFactor * se(0.05));
  EXPECT_NEAR(static_cast<double>(above_2_326) / n, 0.01, kSeToleranceFactor * se(0.01));
  EXPECT_NEAR(static_cast<double>(below_m1_645) / n, 0.05, kSeToleranceFactor * se(0.05));
}

TEST(RiskRng, SameSeedSameSequence) {
  // Pure determinism: two freshly constructed streams with the same seed
  // produce an identical sequence. No hidden state (time, thread id, address
  // of 'this', ...) is allowed to leak in.
  RngStream a(777);
  RngStream b(777);
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(a.next_uniform(), b.next_uniform());
  }
  RngStream c(777), d(777);
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(c.next_standard_normal(), d.next_standard_normal());
  }
}

TEST(RiskRng, DifferentSeedsDiverge) {
  RngStream a(1), b(2);
  bool any_different = false;
  for (int i = 0; i < 16; ++i) {
    if (a.next_uniform() != b.next_uniform()) any_different = true;
  }
  EXPECT_TRUE(any_different);
}

TEST(RiskRng, ChunkSeedIsPureAndWellSeparated) {
  // chunk_seed must be a pure function: calling it for chunk 0..N-1 in
  // forward order gives the same results as calling it in scrambled order
  // (no loop-dependent state), and distinct chunk indices give distinct
  // seeds across a realistic range (an informal collision check, not a
  // uniqueness proof).
  const std::uint64_t global_seed = 0xC0FFEEULL;
  const int n = 5000;

  std::vector<std::uint64_t> forward(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) forward[static_cast<std::size_t>(i)] = chunk_seed(global_seed, static_cast<std::uint64_t>(i));

  std::vector<int> scrambled_order(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) scrambled_order[static_cast<std::size_t>(i)] = n - 1 - i;
  for (int idx : scrambled_order) {
    EXPECT_EQ(chunk_seed(global_seed, static_cast<std::uint64_t>(idx)),
              forward[static_cast<std::size_t>(idx)]);
  }

  std::set<std::uint64_t> distinct(forward.begin(), forward.end());
  EXPECT_EQ(distinct.size(), forward.size()) << "chunk_seed collided within " << n << " chunks";
}

TEST(RiskRng, ChunkSeedNeverDependsOnThreadOrTime) {
  // Not literally testable (there is no thread id or clock reading in the
  // function at all -- see rng.hpp), but this pins the observable contract:
  // calling chunk_seed(seed, i) from different points in time, after
  // unrelated RNG activity on this thread, gives the same answer every time.
  const std::uint64_t s0 = chunk_seed(42, 99);
  SplitMix64 unrelated(123);
  for (int i = 0; i < 1000; ++i) unrelated.next_u64();  // churn, should have zero effect
  const std::uint64_t s1 = chunk_seed(42, 99);
  EXPECT_EQ(s0, s1);
}
