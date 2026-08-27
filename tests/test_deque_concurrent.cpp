// Stage 2 -- ChaseLevDeque under concurrency.
//
// One owner (push + pop) races N thieves (steal). The property is a partition:
// every pushed value comes out exactly once, across {owner pops} u {thief
// steals}. Duplicates, losses, and phantoms each fail a distinct check.
//
// These tests are the ones that matter under `ctest --preset tsan` on Apple
// Silicon: they exercise the acquire / release / seq_cst fences against a weak
// memory model.

#include "taskforge/chase_lev_deque.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using taskforge::ChaseLevDeque;
using taskforge::StealStatus;

namespace {

unsigned thief_count() {
  return std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
}

// Runs the owner/thieves partition test for `items` values and checks it.
void RunPartition(std::int64_t items, int initial_cap) {
  ChaseLevDeque<std::int64_t> deque(initial_cap);
  const unsigned nthieves = thief_count();

  std::atomic<std::int64_t> recovered{0};
  std::atomic<bool> pushing_done{false};
  std::vector<std::vector<std::int64_t>> thief_out(nthieves);
  std::vector<std::int64_t> owner_out;
  owner_out.reserve(static_cast<std::size_t>(items));

  std::vector<std::thread> thieves;
  for (unsigned id = 0; id < nthieves; ++id) {
    thieves.emplace_back([&, id] {
      auto& mine = thief_out[id];
      while (recovered.load(std::memory_order_acquire) < items) {
        std::int64_t v = -1;
        StealStatus s = deque.steal(v);
        if (s == StealStatus::kSuccess) {
          mine.push_back(v);
          recovered.fetch_add(1, std::memory_order_acq_rel);
        } else if (pushing_done.load(std::memory_order_acquire) &&
                   recovered.load(std::memory_order_acquire) >= items) {
          break;
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  for (std::int64_t i = 0; i < items; ++i) {
    deque.push(i);
    if ((i & 7) == 0) {
      if (auto v = deque.pop()) {
        owner_out.push_back(*v);
        recovered.fetch_add(1, std::memory_order_acq_rel);
      }
    }
  }
  pushing_done.store(true, std::memory_order_release);

  while (recovered.load(std::memory_order_acquire) < items) {
    if (auto v = deque.pop()) {
      owner_out.push_back(*v);
      recovered.fetch_add(1, std::memory_order_acq_rel);
    } else {
      std::this_thread::yield();
    }
  }
  for (auto& t : thieves) t.join();

  std::vector<char> seen(static_cast<std::size_t>(items), 0);
  std::int64_t total = 0;
  auto account = [&](const std::vector<std::int64_t>& v) {
    for (std::int64_t x : v) {
      ASSERT_GE(x, 0) << "phantom value";
      ASSERT_LT(x, items) << "phantom value";
      ASSERT_EQ(seen[static_cast<std::size_t>(x)], 0)
          << "duplicate extraction of " << x;
      seen[static_cast<std::size_t>(x)] = 1;
      ++total;
    }
  };
  account(owner_out);
  for (auto& v : thief_out) account(v);

  EXPECT_EQ(total, items);
  for (std::int64_t i = 0; i < items; ++i)
    EXPECT_EQ(seen[static_cast<std::size_t>(i)], 1) << "lost value " << i;
  EXPECT_TRUE(deque.empty());
}

}  // namespace

TEST(DequeConcurrent, PartitionSmallDequeForcesGrowth) {
  RunPartition(/*items=*/50000, /*initial_cap=*/64);
}

TEST(DequeConcurrent, PartitionLargeDequeNoGrowth) {
  RunPartition(/*items=*/200000, /*initial_cap=*/1 << 20);
}

TEST(DequeConcurrent, PartitionRepeatedRuns) {
  // Repeat a moderate run several times; scheduling varies between runs and
  // shakes out interleavings a single pass misses.
  for (int rep = 0; rep < 8; ++rep) RunPartition(/*items=*/20000, /*cap=*/128);
}

TEST(DequeConcurrent, StealOnlyDrainMatchesPushSet) {
  // No owner pops at all: thieves must recover the entire pushed set in order-
  // agnostic fashion, with none lost to a mishandled kAbort.
  ChaseLevDeque<std::int64_t> deque(128);
  const std::int64_t items = 100000;
  const unsigned nthieves = thief_count();

  std::atomic<std::int64_t> recovered{0};
  std::vector<std::vector<std::int64_t>> out(nthieves);
  std::vector<std::thread> thieves;
  for (unsigned id = 0; id < nthieves; ++id) {
    thieves.emplace_back([&, id] {
      while (recovered.load(std::memory_order_acquire) < items) {
        std::int64_t v = -1;
        if (deque.steal(v) == StealStatus::kSuccess) {
          out[id].push_back(v);
          recovered.fetch_add(1, std::memory_order_acq_rel);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }
  for (std::int64_t i = 0; i < items; ++i) deque.push(i);
  for (auto& t : thieves) t.join();

  std::vector<char> seen(static_cast<std::size_t>(items), 0);
  std::int64_t total = 0;
  for (auto& v : out)
    for (std::int64_t x : v) {
      ASSERT_GE(x, 0);
      ASSERT_LT(x, items);
      ASSERT_EQ(seen[static_cast<std::size_t>(x)], 0) << "duplicate " << x;
      seen[static_cast<std::size_t>(x)] = 1;
      ++total;
    }
  EXPECT_EQ(total, items);
  EXPECT_TRUE(deque.empty());
}
