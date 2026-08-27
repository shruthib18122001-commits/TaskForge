// verify_deque_concurrent.cpp -- one owner + N thieves.
//
// Invariant under test: every pushed value is extracted exactly once, by
// exactly one of {owner pop, some thief steal}. No duplicates (same value out
// twice), no losses (value never comes out), no phantoms (a value out that was
// never pushed).
//
// Run this under `make verify-tsan` -- on Apple Silicon it is the real check
// that the acquire/release/seq_cst choices in the deque hold on a weak memory
// model.

#include "taskforge/chase_lev_deque.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "../verify/tf_verify.hpp"

using taskforge::ChaseLevDeque;
using taskforge::StealStatus;

int main() {
  const std::int64_t kItems = 200000;
  const unsigned kThieves =
      std::max(2u, std::min(8u, std::thread::hardware_concurrency()));

  ChaseLevDeque<std::int64_t> deque(256);

  std::atomic<std::int64_t> recovered{0};
  std::atomic<bool> pushing_done{false};

  std::vector<std::vector<std::int64_t>> thief_out(kThieves);
  std::vector<std::int64_t> owner_out;
  owner_out.reserve(static_cast<std::size_t>(kItems));

  auto thief_fn = [&](unsigned id) {
    auto& mine = thief_out[id];
    mine.reserve(static_cast<std::size_t>(kItems / kThieves + 64));
    while (recovered.load(std::memory_order_acquire) < kItems) {
      std::int64_t v = -1;
      StealStatus s = deque.steal(v);
      if (s == StealStatus::kSuccess) {
        mine.push_back(v);
        recovered.fetch_add(1, std::memory_order_acq_rel);
      } else if (pushing_done.load(std::memory_order_acquire) &&
                 recovered.load(std::memory_order_acquire) >= kItems) {
        break;
      } else {
        std::this_thread::yield();
      }
    }
  };

  std::vector<std::thread> thieves;
  for (unsigned i = 0; i < kThieves; ++i) thieves.emplace_back(thief_fn, i);

  // Owner: push everything, interleaving occasional pops so the owner end is
  // genuinely active too, then help drain until all items are accounted for.
  for (std::int64_t i = 0; i < kItems; ++i) {
    deque.push(i);
    if ((i & 7) == 0) {
      if (auto v = deque.pop()) {
        owner_out.push_back(*v);
        recovered.fetch_add(1, std::memory_order_acq_rel);
      }
    }
  }
  pushing_done.store(true, std::memory_order_release);

  while (recovered.load(std::memory_order_acquire) < kItems) {
    if (auto v = deque.pop()) {
      owner_out.push_back(*v);
      recovered.fetch_add(1, std::memory_order_acq_rel);
    } else {
      std::this_thread::yield();
    }
  }

  for (auto& t : thieves) t.join();

  // ---- verify the partition ------------------------------------------------
  std::vector<char> seen(static_cast<std::size_t>(kItems), 0);
  std::int64_t total = 0;

  auto account = [&](const std::vector<std::int64_t>& v) {
    for (std::int64_t x : v) {
      TF_CHECK_MSG(x >= 0 && x < kItems,
                   "phantom value out of range: " + std::to_string(x));
      TF_CHECK_MSG(seen[static_cast<std::size_t>(x)] == 0,
                   "duplicate extraction of value " + std::to_string(x));
      seen[static_cast<std::size_t>(x)] = 1;
      ++total;
    }
  };
  account(owner_out);
  for (auto& v : thief_out) account(v);

  TF_CHECK_EQ(total, kItems);
  for (std::int64_t i = 0; i < kItems; ++i) {
    TF_CHECK_MSG(seen[static_cast<std::size_t>(i)] == 1,
                 "lost value (never extracted): " + std::to_string(i));
  }
  TF_CHECK(deque.empty());

  std::printf("  items=%lld thieves=%u owner_took=%zu grows=%zu\n",
              static_cast<long long>(kItems), kThieves, owner_out.size(),
              deque.grow_count());
  TF_PASS("verify_deque_concurrent");
}
