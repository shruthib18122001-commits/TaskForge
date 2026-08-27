// Stage 8 -- the false-sharing experiment, run deliberately in two directions.
//
// Correctness: both layouts count exactly nthreads * iters.
// Effect: with >= 4 cores writing, the packed layout (adjacent 8-byte atomics
// sharing a cache line) is multiple times slower than alignas(64). This is
// what `alignas(TF_CACHELINE)` on WorkerCounters buys the pool.

#include "taskforge/false_sharing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <thread>

using taskforge::FsLayout;
using taskforge::run_false_sharing;
using taskforge::run_false_sharing_checked;

TEST(FalseSharing, BothLayoutsCountCorrectly) {
  const std::uint64_t iters = 200000;
  for (unsigned n : {1u, 2u, 4u, 8u}) {
    auto p = run_false_sharing_checked(FsLayout::kPacked, n, iters);
    auto a = run_false_sharing_checked(FsLayout::kAligned, n, iters);
    EXPECT_EQ(p.checksum, std::uint64_t{n} * iters) << "packed, n=" << n;
    EXPECT_EQ(a.checksum, std::uint64_t{n} * iters) << "aligned, n=" << n;
  }
}

TEST(FalseSharing, PackedLayoutIsMeasurablySlowerUnderContention) {
  const unsigned hc = std::thread::hardware_concurrency();
  if (hc < 4) GTEST_SKIP() << "need >= 4 hardware threads to show the effect";

  const unsigned n = (hc >= 8) ? 8 : 4;
  const std::uint64_t iters = 20000000;

  run_false_sharing(FsLayout::kAligned, n, iters / 4);  // warm up
  run_false_sharing(FsLayout::kPacked, n, iters / 4);

  double best_packed = 1e9, best_aligned = 1e9;
  for (int r = 0; r < 3; ++r) {
    best_packed = std::min(best_packed, run_false_sharing(FsLayout::kPacked, n, iters));
    best_aligned = std::min(best_aligned, run_false_sharing(FsLayout::kAligned, n, iters));
  }
  const double ratio = best_packed / best_aligned;
  RecordProperty("packed_ms", static_cast<int>(best_packed * 1e3));
  RecordProperty("aligned_ms", static_cast<int>(best_aligned * 1e3));
  RecordProperty("ratio_x10", static_cast<int>(ratio * 10));

  EXPECT_GE(ratio, 2.0)
      << "packed " << best_packed * 1e3 << " ms vs aligned " << best_aligned * 1e3
      << " ms (" << ratio << "x) -- alignas(64) may not be in effect";
}

TEST(FalseSharing, SlotSizes) {
  EXPECT_EQ(sizeof(taskforge::PackedSlot), sizeof(std::atomic<std::uint64_t>));
  EXPECT_EQ(alignof(taskforge::AlignedSlot), std::size_t{TF_CACHELINE});
  EXPECT_EQ(sizeof(taskforge::AlignedSlot), std::size_t{TF_CACHELINE});
}
