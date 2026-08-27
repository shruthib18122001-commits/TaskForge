// verify_deque_stress.cpp -- sustained high-contention stress.
//
// Many short rounds, a deliberately small starting ring so wraparound and
// growth happen constantly, and thieves that never stop hammering. Each round
// independently verifies the exact-once partition. Concurrency bugs that a
// 200k-item single pass misses tend to show up here within a second or two,
// especially under TSan on ARM.
//
// Duration: ~3s by default; override with env TF_STRESS_MS.

#include "taskforge/chase_lev_deque.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include "../verify/tf_verify.hpp"

using taskforge::ChaseLevDeque;
using taskforge::StealStatus;
using clk = std::chrono::steady_clock;

int main() {
  long budget_ms = 3000;
  if (const char* e = std::getenv("TF_STRESS_MS")) {
    long v = std::atol(e);
    if (v > 0) budget_ms = v;
  }
  const unsigned kThieves = std::max(3u, std::thread::hardware_concurrency());
  const std::int64_t kRoundItems = 4096;

  ChaseLevDeque<std::int64_t> deque(64);  // tiny: forces growth every round

  std::atomic<bool> stop{false};
  std::atomic<std::int64_t> round_recovered{0}; // items extracted this round

  // Per-thief, per-round scratch of extracted values (indices within the round).
  std::vector<std::vector<std::int64_t>> thief_out(kThieves);

  auto thief_fn = [&](unsigned id) {
    while (!stop.load(std::memory_order_acquire)) {
      std::int64_t v = -1;
      StealStatus s = deque.steal(v);
      if (s == StealStatus::kSuccess) {
        thief_out[id].push_back(v);
        round_recovered.fetch_add(1, std::memory_order_acq_rel);
      } else {
        std::this_thread::yield();
      }
    }
  };

  std::vector<std::thread> thieves;
  for (unsigned i = 0; i < kThieves; ++i) thieves.emplace_back(thief_fn, i);

  const auto deadline = clk::now() + std::chrono::milliseconds(budget_ms);
  std::int64_t rounds_done = 0;
  std::int64_t base = 0;
  std::vector<std::int64_t> owner_out;

  while (clk::now() < deadline) {
    // ---- start a round ----
    for (auto& v : thief_out) v.clear();
    owner_out.clear();
    round_recovered.store(0, std::memory_order_release);

    // Owner: push kRoundItems unique values [base, base+kRoundItems), with
    // bursts of pops mixed in.
    for (std::int64_t i = 0; i < kRoundItems; ++i) {
      deque.push(base + i);
      if ((i % 5) == 0) {
        if (auto v = deque.pop()) {
          owner_out.push_back(*v);
          round_recovered.fetch_add(1, std::memory_order_acq_rel);
        }
      }
    }
    // Owner helps drain until the whole round is accounted for.
    while (round_recovered.load(std::memory_order_acquire) < kRoundItems) {
      if (auto v = deque.pop()) {
        owner_out.push_back(*v);
        round_recovered.fetch_add(1, std::memory_order_acq_rel);
      } else {
        std::this_thread::yield();
      }
    }

    // ---- verify this round's partition ----
    std::vector<char> seen(static_cast<std::size_t>(kRoundItems), 0);
    std::int64_t total = 0;
    auto account = [&](const std::vector<std::int64_t>& vals) {
      for (std::int64_t x : vals) {
        std::int64_t rel = x - base;
        TF_CHECK_MSG(rel >= 0 && rel < kRoundItems,
                     "phantom/leaked-across-rounds value: " + std::to_string(x) +
                         " (round base " + std::to_string(base) + ")");
        TF_CHECK_MSG(seen[static_cast<std::size_t>(rel)] == 0,
                     "duplicate value " + std::to_string(x));
        seen[static_cast<std::size_t>(rel)] = 1;
        ++total;
      }
    };
    account(owner_out);
    for (auto& v : thief_out) account(v);
    TF_CHECK_EQ(total, kRoundItems);
    TF_CHECK(deque.empty());

    base += kRoundItems;
    ++rounds_done;
  }

  stop.store(true, std::memory_order_release);
  for (auto& t : thieves) t.join();

  TF_CHECK(rounds_done > 0);
  std::printf("  rounds=%lld items/round=%lld thieves=%u total_items=%lld grows=%zu budget=%ldms\n",
              static_cast<long long>(rounds_done),
              static_cast<long long>(kRoundItems), kThieves,
              static_cast<long long>(rounds_done * kRoundItems),
              deque.grow_count(), budget_ms);
  TF_PASS("verify_deque_stress");
}
