// Stage 3 -- work-stealing ThreadPool: submission, futures, recursive submit
// from inside a task, and fork-join via help_until().

#include "taskforge/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <numeric>
#include <vector>

using taskforge::ThreadPool;

TEST(ThreadPool, RunsEverySubmittedTaskExactlyOnce) {
  ThreadPool pool(4);
  const int n = 100000;
  std::vector<std::atomic<int>> hit(n);
  for (auto& h : hit) h.store(0);

  for (int i = 0; i < n; ++i)
    pool.submit([&hit, i] { hit[static_cast<std::size_t>(i)].fetch_add(1); });
  pool.wait_idle();

  for (int i = 0; i < n; ++i)
    EXPECT_EQ(hit[static_cast<std::size_t>(i)].load(), 1) << "task " << i;
  EXPECT_EQ(pool.pending(), 0);
  EXPECT_EQ(pool.exception_count(), 0u);
}

TEST(ThreadPool, SumIsCorrectUnderContention) {
  ThreadPool pool;  // hardware_concurrency
  std::atomic<std::int64_t> sum{0};
  const int n = 200000;
  for (int i = 0; i < n; ++i)
    pool.submit([&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); });
  pool.wait_idle();
  EXPECT_EQ(sum.load(), std::int64_t{n} * (n - 1) / 2);
}

TEST(ThreadPool, FuturesDeliverValues) {
  ThreadPool pool(3);
  std::vector<std::future<int>> fs;
  for (int i = 0; i < 2000; ++i)
    fs.push_back(pool.submit_future([i] { return i * 3; }));
  for (int i = 0; i < 2000; ++i) EXPECT_EQ(fs[static_cast<std::size_t>(i)].get(), i * 3);
}

TEST(ThreadPool, RecursiveSubmitFromInsideTask) {
  ThreadPool pool(4);
  std::atomic<std::int64_t> leaves{0};
  std::function<void(int)> spawn = [&](int d) {
    if (d == 0) {
      leaves.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    pool.submit([&spawn, d] { spawn(d - 1); });
    pool.submit([&spawn, d] { spawn(d - 1); });
  };
  pool.submit([&spawn] { spawn(16); });
  pool.wait_idle();
  EXPECT_EQ(leaves.load(), std::int64_t{1} << 16);
}

namespace {
// Parallel sum with the parent waiting on its children via help_until().
long long par_sum(ThreadPool& pool, const int* a, std::size_t lo, std::size_t hi) {
  if (hi - lo <= 4096) {
    long long s = 0;
    for (std::size_t i = lo; i < hi; ++i) s += a[i];
    return s;
  }
  const std::size_t mid = lo + (hi - lo) / 2;
  long long left = 0, right = 0;
  std::atomic<int> done{0};
  pool.submit([&] {
    left = par_sum(pool, a, lo, mid);
    done.fetch_add(1, std::memory_order_release);
  });
  pool.submit([&] {
    right = par_sum(pool, a, mid, hi);
    done.fetch_add(1, std::memory_order_release);
  });
  pool.help_until([&] { return done.load(std::memory_order_acquire) == 2; });
  return left + right;
}
}  // namespace

TEST(ThreadPool, HelpUntilForkJoinNoDeadlock) {
  ThreadPool pool(6);
  std::vector<int> v(1u << 20);
  std::iota(v.begin(), v.end(), 1);
  const long long want = 1LL * (1u << 20) * ((1u << 20) + 1) / 2;

  long long got = 0;
  pool.submit([&] { got = par_sum(pool, v.data(), 0, v.size()); });
  pool.wait_idle();
  EXPECT_EQ(got, want);
}

TEST(ThreadPool, StatsAccountForAllExecutedTasks) {
  ThreadPool pool(4);
  const int n = 50000;
  for (int i = 0; i < n; ++i) pool.submit([] {});
  pool.wait_idle();

  auto s = pool.stats();
  ASSERT_EQ(s.per_worker.size(), 4u);
  EXPECT_EQ(s.total(&taskforge::WorkerCounters::tasks_executed),
            static_cast<std::uint64_t>(n));
  // Independent tasks from a single EXTERNAL producer all go through the
  // injection queue (Chase-Lev forbids a second writer on a worker's deque),
  // so they are taken via inject_takes, not stolen. This is the "flat workload
  // bypasses the deques" property -- steal-driven balancing is exercised by
  // test_steal_stats.cpp's fork-join / seeded-on-one-worker cases instead.
  EXPECT_GT(s.total(&taskforge::WorkerCounters::inject_takes), 0u);
  EXPECT_EQ(s.total(&taskforge::WorkerCounters::own_pops) +
                s.total(&taskforge::WorkerCounters::steals_success) +
                s.total(&taskforge::WorkerCounters::inject_takes),
            static_cast<std::uint64_t>(n));
}

TEST(ThreadPool, SingleThreadDegenerateCase) {
  ThreadPool pool(1);
  std::atomic<int> c{0};
  for (int i = 0; i < 1000; ++i) pool.submit([&c] { c.fetch_add(1); });
  pool.wait_idle();
  EXPECT_EQ(c.load(), 1000);
}
