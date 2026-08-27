// verify_pool.cpp -- ThreadPool basic behaviour: external submit, recursive
// submit from inside a task, futures, and fork-join via help_until().

#include "taskforge/thread_pool.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <numeric>
#include <vector>

#include "../verify/tf_verify.hpp"

using taskforge::ThreadPool;

static void external_submit_sums() {
  ThreadPool pool(4);
  std::atomic<std::int64_t> sum{0};
  const int n = 100000;
  for (int i = 0; i < n; ++i)
    pool.submit([&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); });
  pool.wait_idle();
  TF_CHECK_EQ(sum.load(), std::int64_t{n} * (n - 1) / 2);
  TF_CHECK_EQ(pool.pending(), std::int64_t{0});
  TF_CHECK_EQ(pool.exception_count(), std::uint64_t{0});
}

static void recursive_submit_from_task() {
  // Each task, until a depth cutoff, submits two children. Total leaves counted.
  ThreadPool pool(4);
  std::atomic<std::int64_t> leaves{0};
  std::function<void(int)> spawn = [&](int depth) {
    if (depth == 0) {
      leaves.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    pool.submit([&spawn, depth] { spawn(depth - 1); });
    pool.submit([&spawn, depth] { spawn(depth - 1); });
  };
  pool.submit([&spawn] { spawn(15); });  // 2^15 leaves
  pool.wait_idle();
  TF_CHECK_EQ(leaves.load(), std::int64_t{1} << 15);
}

static void futures_deliver_values() {
  ThreadPool pool(3);
  std::vector<std::future<int>> fs;
  for (int i = 0; i < 1000; ++i)
    fs.push_back(pool.submit_future([i] { return i * i; }));
  long long got = 0;
  for (int i = 0; i < 1000; ++i) got += fs[static_cast<std::size_t>(i)].get();
  long long want = 0;
  for (int i = 0; i < 1000; ++i) want += 1LL * i * i;
  TF_CHECK_EQ(got, want);
}

// Parallel sum of a vector via recursive fork-join; the parent waits on its
// children with help_until() instead of blocking a worker.
static long long par_sum(ThreadPool& pool, const int* a, std::size_t lo,
                         std::size_t hi) {
  if (hi - lo <= 2048) {
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

static void fork_join_help_until() {
  ThreadPool pool(6);
  std::vector<int> v(1u << 20);
  std::iota(v.begin(), v.end(), 1);
  long long want = 1LL * (1u << 20) * ((1u << 20) + 1) / 2;

  long long got = 0;
  pool.submit([&] { got = par_sum(pool, v.data(), 0, v.size()); });
  pool.wait_idle();
  TF_CHECK_EQ(got, want);
}

int main() {
  external_submit_sums();
  recursive_submit_from_task();
  futures_deliver_values();
  fork_join_help_until();
  TF_PASS("verify_pool");
}
