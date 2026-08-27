// Stage 9 -- the deliberately-wrong deque, as a runnable check.
//
// Labelled "buggy-demo" in CMake and EXCLUDED from the default ctest run
// (CI passes `-LE buggy-demo`). Run it on purpose:  ctest -L buggy-demo
//
// What it asserts depends on the platform, because the bug shows up differently:
//   * weak memory (ARM, ...):  a concurrent stress run corrupts data
//     (torn_reads / duplicate values) -- this test FAILS, which is the point.
//   * x86-64 (TSO):            the stress run stays clean; the real check there
//     is ThreadSanitizer (see `make buggy-demo` / the buggy-ordering-demo CI
//     job). This test SKIPs with a note.
//
// The correct deque passes the identical stress (see test_deque_stress.cpp).

#include "taskforge/chase_lev_deque_relaxed.hpp"
#include "taskforge/cpu_relax.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using taskforge::ChaseLevDequeRelaxed;
using taskforge::StealStatus;
using clk = std::chrono::steady_clock;

namespace {

struct StressResult {
  long long pushed = 0, extracted = 0, torn = 0, duplicates = 0, oob = 0;
  bool corrupted() const {
    return torn > 0 || duplicates > 0 || oob > 0 || (pushed != extracted);
  }
};

StressResult stress_relaxed_deque(int budget_ms, long long unique) {
  std::vector<std::atomic<unsigned char>> seen(static_cast<std::size_t>(unique + 1));
  std::atomic<long long> got{0}, dup{0}, oob{0};
  std::atomic<bool> stop{false};
  auto check = [&](std::uint64_t v) {
    got.fetch_add(1, std::memory_order_relaxed);
    if (v == 0 || v > static_cast<std::uint64_t>(unique)) {
      oob.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (seen[v].fetch_or(1, std::memory_order_relaxed))
      dup.fetch_add(1, std::memory_order_relaxed);
  };

  const unsigned nthieves = std::max(4u, std::thread::hardware_concurrency());
  ChaseLevDequeRelaxed d(64);
  std::vector<std::thread> thieves;
  for (unsigned i = 0; i < nthieves; ++i)
    thieves.emplace_back([&] {
      std::uint64_t v;
      while (!stop.load(std::memory_order_acquire)) {
        if (d.steal(v) == StealStatus::kSuccess) check(v);
        else taskforge::cpu_relax();
      }
    });

  const auto deadline = clk::now() + std::chrono::milliseconds(budget_ms);
  std::uint64_t next = 1;
  while (clk::now() < deadline && next <= static_cast<std::uint64_t>(unique)) {
    for (int j = 0; j < 32 && next <= static_cast<std::uint64_t>(unique); ++j) {
      d.push(next++);
      if (auto o = d.pop()) check(*o);
    }
  }
  for (int s = 0; s < 500; ++s) {
    while (auto o = d.pop()) check(*o);
    std::this_thread::yield();
  }
  stop.store(true, std::memory_order_release);
  for (auto& t : thieves) t.join();
  while (auto o = d.pop()) check(*o);

  StressResult r;
  r.pushed = static_cast<long long>(next - 1);
  r.extracted = got.load();
  r.torn = d.torn_reads();
  r.duplicates = dup.load();
  r.oob = oob.load();
  return r;
}

}  // namespace

TEST(RelaxedDequeDemo, SingleThreadedItStillLooksFine) {
  // The missing release only matters across threads. Alone, it is a normal
  // Chase-Lev deque -- which is exactly why casual testing misses the bug.
  ChaseLevDequeRelaxed d(64);
  for (std::uint64_t i = 1; i <= 50000; ++i) d.push(i);
  long long n = 0, sum = 0;
  std::uint64_t v;
  while (d.steal(v) == StealStatus::kSuccess) { sum += static_cast<long long>(v); ++n; }
  while (auto o = d.pop()) { sum += static_cast<long long>(*o); ++n; }
  EXPECT_EQ(n, 50000);
  EXPECT_EQ(sum, 50000LL * 50001 / 2);
  EXPECT_EQ(d.torn_reads(), 0);
}

TEST(RelaxedDequeDemo, ConcurrentStressCorruptsOnWeakMemory) {
#if defined(__x86_64__) || defined(_M_X64)
  GTEST_SKIP() << "x86-64 is TSO; the stress stays clean here. The check on "
                  "this arch is ThreadSanitizer -- see `make buggy-demo`.";
#else
  const StressResult r = stress_relaxed_deque(/*budget_ms=*/4000, /*unique=*/20'000'000);
  RecordProperty("pushed", static_cast<int>(r.pushed));
  RecordProperty("torn_reads", static_cast<int>(r.torn));
  RecordProperty("duplicates", static_cast<int>(r.duplicates));
  RecordProperty("out_of_bounds", static_cast<int>(r.oob));
  // The bug MUST show itself here. If this ever passes clean, the deque got
  // accidentally fixed (or the compiler/CPU stopped reordering) and the demo
  // has stopped demonstrating.
  EXPECT_TRUE(r.corrupted())
      << "relaxed deque ran clean under stress on a weak-memory target -- "
      << "pushed=" << r.pushed << " extracted=" << r.extracted
      << " torn=" << r.torn << " dups=" << r.duplicates << " oob=" << r.oob;
#endif
}
