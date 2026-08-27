// verify_pool_shutdown.cpp -- clean shutdown in the three states the spec calls
// out: tasks in flight, tasks queued, workers mid-steal. Plus shutdown_now()
// discarding queued work.

#include "taskforge/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "../verify/tf_verify.hpp"

using taskforge::ThreadPool;
using namespace std::chrono_literals;

// (1) Destruct with tasks still running. Graceful shutdown must let them finish.
static void join_with_tasks_in_flight() {
  std::atomic<int> started{0}, finished{0};
  {
    ThreadPool pool(4);
    for (int i = 0; i < 4; ++i) {
      pool.submit([&] {
        started.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(80ms);  // still running when we destruct
        finished.fetch_add(1, std::memory_order_relaxed);
      });
    }
    while (started.load() < 4) std::this_thread::yield();
    // ~ThreadPool() here, with all 4 tasks mid-sleep.
  }
  TF_CHECK_EQ(finished.load(), 4);
}

// (2) Destruct with many tasks queued and not yet started. Graceful shutdown
// runs them all before workers exit.
static void join_with_tasks_queued() {
  std::atomic<std::int64_t> ran{0};
  const int n = 20000;
  {
    ThreadPool pool(4);
    for (int i = 0; i < n; ++i)
      pool.submit([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    // No wait_idle(): destruct immediately with a deep backlog.
  }
  TF_CHECK_EQ(ran.load(), std::int64_t{n});
}

// (3) Destruct under active contention, workers mid-steal.
static void join_while_stealing() {
  std::atomic<std::int64_t> ran{0};
  // `fan` must OUTLIVE `pool`: workers run tasks that call fan() right up until
  // ~ThreadPool has joined them, so anything those tasks capture by reference
  // has to be declared before the pool (destroyed after it). Declaring the pool
  // last, or calling wait_idle()/shutdown() before the captured state dies, is
  // the rule for any fork-join workload.
  std::function<void(int)> fan;
  {
    ThreadPool pool(8);
    fan = [&](int d) {
      ran.fetch_add(1, std::memory_order_relaxed);
      if (d == 0) return;
      pool.submit([&fan, d] { fan(d - 1); });
      pool.submit([&fan, d] { fan(d - 1); });
    };
    for (int i = 0; i < 8; ++i) pool.submit([&fan] { fan(12); });
    std::this_thread::sleep_for(5ms);  // let the storm get going
    // ~ThreadPool() mid-storm: graceful shutdown drains what is queued; any
    // recursive submit() that races the shutdown flag is rejected (counted).
  }
  TF_CHECK(ran.load() > 0);
}

// shutdown_now(): abandons queued work, exits fast, no leak (ASan will tell).
static void shutdown_now_discards() {
  std::atomic<std::int64_t> ran{0};
  ThreadPool pool(4);
  for (int i = 0; i < 100000; ++i)
    pool.submit([&] {
      std::this_thread::sleep_for(1ms);
      ran.fetch_add(1, std::memory_order_relaxed);
    });
  pool.shutdown_now();
  // A handful may have run; the vast majority were discarded unrun.
  TF_CHECK(ran.load() < 100000);
  TF_CHECK_EQ(pool.pending(), std::int64_t{0});  // accounting still balances
}

// submit() after shutdown is rejected, not silently dropped.
static void submit_after_shutdown_throws() {
  ThreadPool pool(2);
  pool.shutdown();
  bool threw = false;
  try {
    pool.submit([] {});
  } catch (const std::runtime_error&) {
    threw = true;
  }
  TF_CHECK(threw);
}

int main() {
  join_with_tasks_in_flight();
  join_with_tasks_queued();
  join_while_stealing();
  shutdown_now_discards();
  submit_after_shutdown_throws();
  TF_PASS("verify_pool_shutdown");
}
