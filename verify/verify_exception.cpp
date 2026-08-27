// verify_exception.cpp -- a task that throws must not take the pool down.
// Policy: the worker catches it, counts it, retains the first one; every other
// task still runs; wait_idle() still returns; submit_future() delivers the
// exception through the future.

#include "taskforge/thread_pool.hpp"

#include <atomic>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <string>

#include "../verify/tf_verify.hpp"

using taskforge::ThreadPool;

static void throwing_tasks_do_not_break_the_pool() {
  ThreadPool pool(4);
  std::atomic<std::int64_t> ok_ran{0};
  const int good = 5000;
  const int bad = 200;

  for (int i = 0; i < good; ++i)
    pool.submit([&] { ok_ran.fetch_add(1, std::memory_order_relaxed); });
  for (int i = 0; i < bad; ++i)
    pool.submit([] { throw std::runtime_error("boom"); });
  for (int i = 0; i < good; ++i)
    pool.submit([&] { ok_ran.fetch_add(1, std::memory_order_relaxed); });

  pool.wait_idle();  // must return despite the throwers

  TF_CHECK_EQ(ok_ran.load(), std::int64_t{2 * good});
  TF_CHECK_EQ(pool.exception_count(), static_cast<std::uint64_t>(bad));
  TF_CHECK(pool.first_exception() != nullptr);

  // The retained exception is rethrowable and carries the message.
  bool matched = false;
  try {
    std::rethrow_exception(pool.first_exception());
  } catch (const std::runtime_error& e) {
    matched = std::string(e.what()) == "boom";
  }
  TF_CHECK(matched);
}

static void exception_reaches_the_future() {
  ThreadPool pool(2);
  auto f = pool.submit_future([]() -> int { throw std::logic_error("nope"); });
  bool matched = false;
  try {
    f.get();
  } catch (const std::logic_error& e) {
    matched = std::string(e.what()) == "nope";
  }
  TF_CHECK(matched);

  // Pool is still fully usable afterwards.
  auto g = pool.submit_future([] { return 21 + 21; });
  TF_CHECK_EQ(g.get(), 42);
}

static void recursive_task_that_throws_is_contained() {
  ThreadPool pool(4);
  std::atomic<std::int64_t> leaves{0};
  std::function<void(int)> spawn = [&](int d) {
    if (d == 0) {
      leaves.fetch_add(1, std::memory_order_relaxed);
      if ((leaves.load() % 7) == 0) throw std::runtime_error("leaf blew up");
      return;
    }
    pool.submit([&spawn, d] { spawn(d - 1); });
    pool.submit([&spawn, d] { spawn(d - 1); });
  };
  pool.submit([&spawn] { spawn(12) ; });
  pool.wait_idle();

  TF_CHECK_EQ(leaves.load(), std::int64_t{1} << 12);
  TF_CHECK(pool.exception_count() > 0);
}

int main() {
  throwing_tasks_do_not_break_the_pool();
  exception_reaches_the_future();
  recursive_task_that_throws_is_contained();
  TF_PASS("verify_exception");
}
