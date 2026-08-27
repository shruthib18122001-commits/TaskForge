// verify_steal_stats.cpp -- Stage 7 per-worker instrumentation is consistent.
//
//   * every executed task is attributed to exactly one acquisition path:
//         own_pops + steals_success + inject_takes == tasks_executed  (per worker)
//   * totals add up:  sum(tasks_executed) == tasks submitted
//   * when all work is seeded onto one worker, the rest genuinely steal it
//   * idle workers record spin / sleep

#include "taskforge/thread_pool.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

#include "../verify/tf_verify.hpp"

using taskforge::ThreadPool;
using taskforge::WorkerCounters;

static void attribution_identity_holds() {
  ThreadPool pool(6);
  const int n = 120000;
  std::atomic<int> ran{0};
  for (int i = 0; i < n; ++i) pool.submit([&ran] { ran.fetch_add(1); });
  pool.wait_idle();
  TF_CHECK_EQ(ran.load(), n);

  const auto s = pool.stats();
  std::uint64_t total_exec = 0;
  for (const auto& c : s.per_worker) {
    TF_CHECK_MSG(c.own_pops + c.steals_success + c.inject_takes == c.tasks_executed,
                 "worker attribution mismatch: pops+steals+inject=" +
                     std::to_string(c.own_pops + c.steals_success + c.inject_takes) +
                     " executed=" + std::to_string(c.tasks_executed));
    total_exec += c.tasks_executed;
  }
  TF_CHECK_EQ(total_exec, static_cast<std::uint64_t>(n));
}

static void seeded_on_one_worker_is_stolen_by_the_rest() {
  ThreadPool pool(8);
  const int n = 40000;
  std::atomic<int> ran{0};
  // A single generator task loops submit(): all n children land on ITS deque.
  pool.submit([&] {
    for (int i = 0; i < n; ++i) pool.submit([&ran] { ran.fetch_add(1); });
  });
  pool.wait_idle();
  TF_CHECK_EQ(ran.load(), n);

  const auto s = pool.stats();
  std::uint64_t stolen = s.total(&WorkerCounters::steals_success);
  std::uint64_t popped = s.total(&WorkerCounters::own_pops);
  // The 7 non-generator workers should have stolen the bulk of the work.
  TF_CHECK_MSG(stolen > static_cast<std::uint64_t>(n) / 2,
               "expected most of " + std::to_string(n) +
                   " tasks to be stolen, got " + std::to_string(stolen));
  TF_CHECK_EQ(stolen + popped + s.total(&WorkerCounters::inject_takes),
             static_cast<std::uint64_t>(n) + 1);  // +1 for the generator itself
}

static void idle_workers_record_backoff() {
  // Far more workers than sustained work -> most of them spin / sleep.
  ThreadPool pool(8);
  for (int round = 0; round < 20; ++round) {
    pool.submit([] {});  // one tiny task, then everyone goes idle
    pool.wait_idle();
  }
  const auto s = pool.stats();
  const std::uint64_t spin = s.total(&WorkerCounters::spin_iters);
  const std::uint64_t sleeps = s.total(&WorkerCounters::sleeps);
  TF_CHECK_MSG(spin + sleeps > 0, "no backoff recorded despite idle workers");
}

int main() {
  attribution_identity_holds();
  seeded_on_one_worker_is_stolen_by_the_rest();
  idle_workers_record_backoff();
  TF_PASS("verify_steal_stats");
}
