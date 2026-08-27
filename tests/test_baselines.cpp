// Stage 4 -- the baseline pools. They exist to be measured against, so the bar
// is only that they honour the same contract: run every task once, correct
// results, contained exceptions, clean shutdown. The same battery runs against
// ThreadPool too, as a cross-check that the battery itself is meaningful.

#include "taskforge/async_pool.hpp"
#include "taskforge/mutex_queue_pool.hpp"
#include "taskforge/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>

template <class Pool>
class PoolContract : public ::testing::Test {};

using PoolTypes = ::testing::Types<taskforge::ThreadPool, taskforge::MutexQueuePool,
                                   taskforge::AsyncPool>;
// GoogleTest's TYPED_TEST_SUITE takes an optional 3rd (name-generator) arg;
// omitting it leaves __VA_ARGS__ empty, which -Wpedantic flags as a C++20
// extension under -std=c++17. GoogleTest's macro, not our code.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
TYPED_TEST_SUITE(PoolContract, PoolTypes);
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// AsyncPool spawns a thread per task; keep its counts modest.
namespace {
template <class Pool>
constexpr int scale_for() {
  return std::is_same<Pool, taskforge::AsyncPool>::value ? 1500 : 40000;
}
}  // namespace

TYPED_TEST(PoolContract, RunsEveryTaskAndSumsCorrectly) {
  const int n = scale_for<TypeParam>();
  TypeParam pool(4);
  std::atomic<std::int64_t> sum{0};
  for (int i = 0; i < n; ++i)
    pool.submit([&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); });
  pool.wait_idle();
  EXPECT_EQ(sum.load(), std::int64_t{n} * (n - 1) / 2);
  EXPECT_EQ(pool.pending(), 0);
}

TYPED_TEST(PoolContract, FuturesDeliverValues) {
  TypeParam pool(3);
  auto f = pool.submit_future([] { return 6 * 7; });
  EXPECT_EQ(f.get(), 42);
}

TYPED_TEST(PoolContract, ThrowingTaskIsContained) {
  const int n = scale_for<TypeParam>();
  TypeParam pool(4);
  std::atomic<std::int64_t> ok{0};
  for (int i = 0; i < n; ++i) pool.submit([&ok] { ok.fetch_add(1); });
  pool.submit([] { throw std::runtime_error("boom"); });
  for (int i = 0; i < n; ++i) pool.submit([&ok] { ok.fetch_add(1); });
  pool.wait_idle();
  EXPECT_EQ(ok.load(), std::int64_t{2 * n});
  EXPECT_EQ(pool.exception_count(), 1u);
  EXPECT_NE(pool.first_exception(), nullptr);
}

TYPED_TEST(PoolContract, SubmitAfterShutdownThrows) {
  TypeParam pool(2);
  pool.shutdown();
  EXPECT_THROW(pool.submit([] {}), std::runtime_error);
}

// A property specific to the mutex baseline: no per-worker stealing exists, so
// stats() is a zeroed block of the right size (callers still template over it).
TEST(MutexQueuePoolSpecific, StatsIsZeroedButSized) {
  taskforge::MutexQueuePool pool(5);
  pool.submit([] {});
  pool.wait_idle();
  auto s = pool.stats();
  EXPECT_EQ(s.per_worker.size(), 5u);
  EXPECT_EQ(s.total(&taskforge::WorkerCounters::tasks_executed), 0u);
}
