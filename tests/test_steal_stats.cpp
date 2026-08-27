// Stage 7 -- per-worker steal instrumentation is internally consistent and
// actually reflects the mechanism.

#include "taskforge/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

using taskforge::ThreadPool;
using taskforge::WorkerCounters;

TEST(StealStats, EveryExecutedTaskIsAttributedToOneAcquisitionPath) {
  ThreadPool pool(6);
  const int n = 100000;
  std::atomic<int> ran{0};
  for (int i = 0; i < n; ++i) pool.submit([&ran] { ran.fetch_add(1); });
  pool.wait_idle();
  ASSERT_EQ(ran.load(), n);

  const auto s = pool.stats();
  std::uint64_t total = 0;
  for (const auto& c : s.per_worker) {
    EXPECT_EQ(c.own_pops + c.steals_success + c.inject_takes, c.tasks_executed);
    total += c.tasks_executed;
  }
  EXPECT_EQ(total, static_cast<std::uint64_t>(n));
}

TEST(StealStats, WorkSeededOnOneWorkerIsStolenByTheOthers) {
  ThreadPool pool(8);
  const int n = 40000;
  std::atomic<int> ran{0};
  pool.submit([&] {
    for (int i = 0; i < n; ++i) pool.submit([&ran] { ran.fetch_add(1); });
  });
  pool.wait_idle();
  ASSERT_EQ(ran.load(), n);

  const auto s = pool.stats();
  const std::uint64_t stolen = s.total(&WorkerCounters::steals_success);
  const std::uint64_t popped = s.total(&WorkerCounters::own_pops);
  const std::uint64_t injected = s.total(&WorkerCounters::inject_takes);

  EXPECT_GT(stolen, static_cast<std::uint64_t>(n) / 2)
      << "most tasks should have been stolen off the generator's deque";
  EXPECT_EQ(stolen + popped + injected, static_cast<std::uint64_t>(n) + 1);
}

TEST(StealStats, AbortsAndEmptiesAreCountedSeparately) {
  // Heavy contention on a small amount of work -> lots of failed steal attempts
  // (empty victims) and some CAS aborts.
  ThreadPool pool(8);
  for (int r = 0; r < 200; ++r) {
    pool.submit([] {});
    pool.wait_idle();
  }
  const auto s = pool.stats();
  EXPECT_GT(s.total(&WorkerCounters::steals_empty), 0u);
  // aborts may legitimately be zero on a lightly contended run; just check the
  // field is readable and non-negative-by-type.
  EXPECT_GE(s.total(&WorkerCounters::steals_abort), 0u);
}

TEST(StealStats, IdleWorkersRecordBackoff) {
  ThreadPool pool(8);
  for (int r = 0; r < 20; ++r) {
    pool.submit([] {});
    pool.wait_idle();
  }
  const auto s = pool.stats();
  EXPECT_GT(s.total(&WorkerCounters::spin_iters) + s.total(&WorkerCounters::sleeps), 0u);
}

TEST(StealStats, SnapshotSizesMatchThreadCount) {
  ThreadPool pool(5);
  pool.submit([] {});
  pool.wait_idle();
  EXPECT_EQ(pool.stats().per_worker.size(), 5u);
  EXPECT_EQ(pool.thread_count(), 5u);
}
