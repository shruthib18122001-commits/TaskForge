// Stage 3 -- clean shutdown. The spec calls out three join states explicitly:
// tasks in flight, tasks queued, workers mid-steal. Plus shutdown_now().

#include "taskforge/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

using taskforge::ThreadPool;
using namespace std::chrono_literals;

TEST(PoolShutdown, JoinWithTasksInFlight) {
  std::atomic<int> started{0}, finished{0};
  {
    ThreadPool pool(4);
    for (int i = 0; i < 4; ++i)
      pool.submit([&] {
        started.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(60ms);
        finished.fetch_add(1, std::memory_order_relaxed);
      });
    while (started.load() < 4) std::this_thread::yield();
    // ~ThreadPool with all four tasks mid-sleep.
  }
  EXPECT_EQ(finished.load(), 4);
}

TEST(PoolShutdown, JoinWithDeepBacklogRunsItAll) {
  std::atomic<std::int64_t> ran{0};
  const int n = 40000;
  {
    ThreadPool pool(4);
    for (int i = 0; i < n; ++i)
      pool.submit([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    // Destruct immediately, no wait_idle().
  }
  EXPECT_EQ(ran.load(), std::int64_t{n});
}

TEST(PoolShutdown, JoinWhileWorkersAreStealing) {
  std::atomic<std::int64_t> ran{0};
  std::function<void(int)> fan;  // must outlive the pool (see thread_pool.hpp)
  {
    ThreadPool pool(8);
    fan = [&](int d) {
      ran.fetch_add(1, std::memory_order_relaxed);
      if (d == 0) return;
      pool.submit([&fan, d] { fan(d - 1); });
      pool.submit([&fan, d] { fan(d - 1); });
    };
    for (int i = 0; i < 8; ++i) pool.submit([&fan] { fan(12); });
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_GT(ran.load(), 0);
}

TEST(PoolShutdown, ShutdownNowDiscardsQueuedAndBalancesAccounting) {
  std::atomic<std::int64_t> ran{0};
  ThreadPool pool(4);
  for (int i = 0; i < 100000; ++i)
    pool.submit([&] {
      std::this_thread::sleep_for(1ms);
      ran.fetch_add(1, std::memory_order_relaxed);
    });
  pool.shutdown_now();
  EXPECT_LT(ran.load(), 100000);   // most were dropped unrun
  EXPECT_EQ(pool.pending(), 0);    // discarded tasks still decremented
}

TEST(PoolShutdown, SubmitAfterShutdownThrows) {
  ThreadPool pool(2);
  pool.shutdown();
  EXPECT_THROW(pool.submit([] {}), std::runtime_error);
}

TEST(PoolShutdown, DoubleShutdownIsHarmless) {
  ThreadPool pool(2);
  pool.submit([] {});
  pool.shutdown();
  pool.shutdown();  // no-op second time
  SUCCEED();
}
