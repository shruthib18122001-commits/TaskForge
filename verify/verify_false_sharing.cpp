// verify_false_sharing.cpp -- Stage 8. Packed per-thread counters (sharing
// cache lines) are measurably slower than alignas(64) ones once more than one
// core is writing.
//
// Timing-based, so the margin is generous and the check is skipped on machines
// with too few cores to show the effect.

#include "taskforge/false_sharing.hpp"

#include <cstdint>
#include <cstdio>
#include <thread>

#include "../verify/tf_verify.hpp"

using taskforge::FsLayout;
using taskforge::run_false_sharing;

int main() {
  const unsigned hc = std::thread::hardware_concurrency();

  // Both layouts run cleanly across a range of thread counts.
  for (unsigned k = 1; k <= (hc ? hc : 4u); ++k) {
    TF_CHECK(run_false_sharing(FsLayout::kPacked, k, 100000) >= 0.0);
    TF_CHECK(run_false_sharing(FsLayout::kAligned, k, 100000) >= 0.0);
  }

  if (hc < 4) {
    std::printf("  (only %u hardware threads; skipping the timing assertion)\n", hc);
    TF_PASS("verify_false_sharing");
  }

  const unsigned n = (hc >= 8) ? 8 : 4;
  const std::uint64_t iters = 30000000;

  run_false_sharing(FsLayout::kAligned, n, iters / 4);  // warm up
  run_false_sharing(FsLayout::kPacked, n, iters / 4);

  double best_packed = 1e9, best_aligned = 1e9;
  for (int r = 0; r < 3; ++r) {
    const double p = run_false_sharing(FsLayout::kPacked, n, iters);
    const double a = run_false_sharing(FsLayout::kAligned, n, iters);
    if (p < best_packed) best_packed = p;
    if (a < best_aligned) best_aligned = a;
  }

  const double ratio = best_packed / best_aligned;
  std::printf("  threads=%u  packed=%.1f ms  aligned=%.1f ms  packed is %.1fx slower\n",
              n, best_packed * 1e3, best_aligned * 1e3, ratio);

  // Conservative bar: false sharing should cost at least 2x here. On the dev
  // machine (Apple M5) it is ~20-50x; 2x leaves headroom for a noisy CI box
  // while still failing if alignas(64) silently stops taking effect.
  TF_CHECK_MSG(ratio >= 2.0,
               "packed only " + std::to_string(ratio) +
                   "x slower than aligned -- alignas(64) may not be in effect");

  TF_PASS("verify_false_sharing");
}
