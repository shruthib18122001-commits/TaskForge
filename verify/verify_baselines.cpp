// verify_baselines.cpp -- the two baseline pools honour the same contract as
// ThreadPool: every task runs once, sums are correct, exceptions are contained,
// wait_idle() returns, shutdown is clean.

#include "taskforge/async_pool.hpp"
#include "taskforge/mutex_queue_pool.hpp"
#include "taskforge/thread_pool.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "../verify/tf_verify.hpp"

using taskforge::AsyncPool;
using taskforge::MutexQueuePool;
using taskforge::ThreadPool;

// One battery, run against whichever pool type. `scale` keeps the AsyncPool
// (thread-per-task) workload sane.
template <class Pool>
static void battery(const char* name, int scale) {
  // (a) every task runs exactly once; sum is correct
  {
    Pool pool(4);
    std::atomic<std::int64_t> sum{0};
    const int n = scale;
    for (int i = 0; i < n; ++i)
      pool.submit([&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); });
    pool.wait_idle();
    TF_CHECK_EQ(sum.load(), std::int64_t{n} * (n - 1) / 2);
    TF_CHECK_EQ(pool.pending(), std::int64_t{0});
  }
  // (b) futures
  {
    Pool pool(3);
    auto f = pool.submit_future([] { return 6 * 7; });
    TF_CHECK_EQ(f.get(), 42);
  }
  // (c) a throwing task does not break the pool
  {
    Pool pool(4);
    std::atomic<std::int64_t> ok{0};
    for (int i = 0; i < scale; ++i) pool.submit([&ok] { ok.fetch_add(1); });
    pool.submit([] { throw std::runtime_error("boom"); });
    for (int i = 0; i < scale; ++i) pool.submit([&ok] { ok.fetch_add(1); });
    pool.wait_idle();
    TF_CHECK_EQ(ok.load(), std::int64_t{2 * scale});
    TF_CHECK_EQ(pool.exception_count(), std::uint64_t{1});
    TF_CHECK(pool.first_exception() != nullptr);
  }
  // (d) submit after shutdown is rejected
  {
    Pool pool(2);
    pool.shutdown();
    bool threw = false;
    try {
      pool.submit([] {});
    } catch (const std::runtime_error&) {
      threw = true;
    }
    TF_CHECK(threw);
  }
  std::printf("  %s: battery ok (scale=%d)\n", name, scale);
}

int main() {
  battery<ThreadPool>("ThreadPool", 50000);       // sanity: same battery passes for the real one
  battery<MutexQueuePool>("MutexQueuePool", 50000);
  battery<AsyncPool>("AsyncPool", 1500);           // thread-per-task: keep it small
  TF_PASS("verify_baselines");
}
