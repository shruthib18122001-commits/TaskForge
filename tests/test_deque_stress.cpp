// Stage 2 -- ChaseLevDeque sustained stress.
//
// Short rounds, a tiny starting ring (constant wraparound + growth), thieves
// that never idle. Runs for a wall-clock budget rather than a fixed iteration
// count so a slow CI box still gets real coverage. Each round independently
// checks the exact-once partition.
//
// Budget: 2s default; env TF_STRESS_MS overrides (CI leaves the default).

#include "taskforge/chase_lev_deque.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

using taskforge::ChaseLevDeque;
using taskforge::StealStatus;
using clk = std::chrono::steady_clock;

TEST(DequeStress, SustainedContentionPartitionHolds) {
  long budget_ms = 2000;
  if (const char* e = std::getenv("TF_STRESS_MS")) {
    long v = std::atol(e);
    if (v > 0) budget_ms = v;
  }
  const unsigned nthieves = std::max(3u, std::thread::hardware_concurrency());
  const std::int64_t round_items = 4096;

  ChaseLevDeque<std::int64_t> deque(64);
  std::atomic<bool> stop{false};
  std::atomic<std::int64_t> round_recovered{0};
  std::vector<std::vector<std::int64_t>> thief_out(nthieves);

  std::vector<std::thread> thieves;
  for (unsigned id = 0; id < nthieves; ++id) {
    thieves.emplace_back([&, id] {
      while (!stop.load(std::memory_order_acquire)) {
        std::int64_t v = -1;
        if (deque.steal(v) == StealStatus::kSuccess) {
          thief_out[id].push_back(v);
          round_recovered.fetch_add(1, std::memory_order_acq_rel);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  const auto deadline = clk::now() + std::chrono::milliseconds(budget_ms);
  std::int64_t base = 0;
  std::int64_t rounds = 0;
  std::vector<std::int64_t> owner_out;

  while (clk::now() < deadline) {
    for (auto& v : thief_out) v.clear();
    owner_out.clear();
    round_recovered.store(0, std::memory_order_release);

    for (std::int64_t i = 0; i < round_items; ++i) {
      deque.push(base + i);
      if ((i % 5) == 0) {
        if (auto v = deque.pop()) {
          owner_out.push_back(*v);
          round_recovered.fetch_add(1, std::memory_order_acq_rel);
        }
      }
    }
    while (round_recovered.load(std::memory_order_acquire) < round_items) {
      if (auto v = deque.pop()) {
        owner_out.push_back(*v);
        round_recovered.fetch_add(1, std::memory_order_acq_rel);
      } else {
        std::this_thread::yield();
      }
    }

    std::vector<char> seen(static_cast<std::size_t>(round_items), 0);
    std::int64_t total = 0;
    auto account = [&](const std::vector<std::int64_t>& vals) {
      for (std::int64_t x : vals) {
        const std::int64_t rel = x - base;
        ASSERT_GE(rel, 0) << "value " << x << " leaked across rounds";
        ASSERT_LT(rel, round_items) << "value " << x << " leaked across rounds";
        ASSERT_EQ(seen[static_cast<std::size_t>(rel)], 0) << "duplicate " << x;
        seen[static_cast<std::size_t>(rel)] = 1;
        ++total;
      }
    };
    account(owner_out);
    for (auto& v : thief_out) account(v);
    ASSERT_EQ(total, round_items);
    ASSERT_TRUE(deque.empty());

    base += round_items;
    ++rounds;
  }

  stop.store(true, std::memory_order_release);
  for (auto& t : thieves) t.join();

  EXPECT_GT(rounds, 0);
  RecordProperty("rounds", static_cast<int>(rounds));
  RecordProperty("total_items", static_cast<int>(rounds * round_items));
  RecordProperty("grows", static_cast<int>(deque.grow_count()));
}
