// Stage 3 -- exception policy. A throwing task is caught, counted, and its
// exception retained (first one); the pool keeps running, wait_idle() still
// returns, and submit_future() delivers the exception to the caller.

#include "taskforge/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

using taskforge::ThreadPool;

TEST(ExceptionSafety, ThrowingTasksDoNotStopThePool) {
  ThreadPool pool(4);
  std::atomic<std::int64_t> ok{0};
  const int good = 5000, bad = 250;

  for (int i = 0; i < good; ++i) pool.submit([&] { ok.fetch_add(1); });
  for (int i = 0; i < bad; ++i) pool.submit([] { throw std::runtime_error("boom"); });
  for (int i = 0; i < good; ++i) pool.submit([&] { ok.fetch_add(1); });

  pool.wait_idle();

  EXPECT_EQ(ok.load(), std::int64_t{2 * good});
  EXPECT_EQ(pool.exception_count(), static_cast<std::uint64_t>(bad));
  ASSERT_NE(pool.first_exception(), nullptr);
  EXPECT_THROW(std::rethrow_exception(pool.first_exception()), std::runtime_error);
}

TEST(ExceptionSafety, NonStdExceptionAlsoContained) {
  ThreadPool pool(2);
  pool.submit([] { throw 42; });          // not derived from std::exception
  pool.submit([] { throw std::string("s"); });
  pool.wait_idle();
  EXPECT_EQ(pool.exception_count(), 2u);
}

TEST(ExceptionSafety, ExceptionPropagatesThroughFuture) {
  ThreadPool pool(2);
  auto f = pool.submit_future([]() -> int { throw std::logic_error("nope"); });
  try {
    f.get();
    FAIL() << "expected the exception to come through the future";
  } catch (const std::logic_error& e) {
    EXPECT_EQ(std::string(e.what()), "nope");
  }
  // Pool still usable.
  EXPECT_EQ(pool.submit_future([] { return 7; }).get(), 7);
}

TEST(ExceptionSafety, RecursiveWorkloadWithScatteredThrows) {
  ThreadPool pool(4);
  std::atomic<std::int64_t> leaves{0};
  std::function<void(int)> spawn = [&](int d) {
    if (d == 0) {
      auto n = leaves.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n % 11 == 0) throw std::runtime_error("leaf");
      return;
    }
    pool.submit([&spawn, d] { spawn(d - 1); });
    pool.submit([&spawn, d] { spawn(d - 1); });
  };
  pool.submit([&spawn] { spawn(13); });
  pool.wait_idle();

  EXPECT_EQ(leaves.load(), std::int64_t{1} << 13);
  EXPECT_GT(pool.exception_count(), 0u);
  EXPECT_EQ(pool.pending(), 0);
}
