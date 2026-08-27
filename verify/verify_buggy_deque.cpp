// verify_buggy_deque.cpp -- Stage 9 demonstration harness.
//
// Stresses ChaseLevDequeRelaxed (the deliberately-wrong deque). Each element is
// a two-word cell { v, v_copy }; a correct deque always hands both words back
// consistently. This one drops the release store in push(), so a thief can see
// the incremented bottom_ and read a cell that is only half-published.
//
// Signals of the bug:
//   * d.torn_reads() > 0        -- a thief saw v != v_copy
//   * duplicate extracted value -- a stale slot re-read
//   * ThreadSanitizer: data race on the cell (build with -fsanitize=thread)
//
// Expected: normal build PASSES on x86 (TSO) and often on ARM too -- casual
// testing does not reliably catch this. TSan reports the race. `make buggy-demo`
// runs the TSan build; CI job `buggy-ordering-demo` fails if it comes back clean.
//
// Env: TF_BUG_MS (default 3000), TF_BUG_UNIQUE (value space, default 20,000,000).

#include "taskforge/chase_lev_deque_relaxed.hpp"
#include "taskforge/cpu_relax.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using taskforge::ChaseLevDequeRelaxed;
using taskforge::StealStatus;
using clk = std::chrono::steady_clock;

#if defined(__SANITIZE_THREAD__)
#define TF_UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TF_UNDER_TSAN 1
#endif
#endif
#ifndef TF_UNDER_TSAN
#define TF_UNDER_TSAN 0
#endif

int main() {
  long budget_ms = 3000;
  long long unique = 20'000'000;
  if (const char* e = std::getenv("TF_BUG_MS")) { long v = std::atol(e); if (v > 0) budget_ms = v; }
  if (const char* e = std::getenv("TF_BUG_UNIQUE")) { long long v = std::atoll(e); if (v > 0) unique = v; }

  std::vector<std::atomic<unsigned char>> seen(static_cast<std::size_t>(unique + 1));
  std::atomic<long long> got{0}, dup{0}, oob{0};
  std::atomic<bool> stop{false};

  auto check = [&](std::uint64_t v) {
    got.fetch_add(1, std::memory_order_relaxed);
    if (v == 0 || v > static_cast<std::uint64_t>(unique)) {
      oob.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (seen[v].fetch_or(1, std::memory_order_relaxed))
      dup.fetch_add(1, std::memory_order_relaxed);
  };

  // Keep the deque near-empty so owner push and thief steal fight over the same
  // cell -- that is where the missing release bites.
  const unsigned nthieves =
      TF_UNDER_TSAN ? 2u : std::max(4u, std::thread::hardware_concurrency());
  ChaseLevDequeRelaxed d(64);

  std::vector<std::thread> thieves;
  for (unsigned i = 0; i < nthieves; ++i)
    thieves.emplace_back([&] {
      std::uint64_t v;
      while (!stop.load(std::memory_order_acquire)) {
        if (d.steal(v) == StealStatus::kSuccess) check(v);
        else taskforge::cpu_relax();
      }
    });

  const auto deadline = clk::now() + std::chrono::milliseconds(budget_ms);
  std::uint64_t next = 1;
  while (clk::now() < deadline && next <= static_cast<std::uint64_t>(unique)) {
    for (int j = 0; j < 32 && next <= static_cast<std::uint64_t>(unique); ++j) {
      d.push(next++);
      if (auto o = d.pop()) check(*o);  // near-empty -> same-cell contention
    }
  }
  for (int spin = 0; spin < 500; ++spin) {
    while (auto o = d.pop()) check(*o);
    std::this_thread::yield();
  }
  stop.store(true, std::memory_order_release);
  for (auto& t : thieves) t.join();
  while (auto o = d.pop()) check(*o);

  const long long pushed = static_cast<long long>(next - 1);
  const long long lost = pushed - got.load();
  std::printf("pushed=%lld extracted=%lld lost=%lld torn_reads=%lld "
              "duplicates=%lld out-of-bounds=%lld\n",
              pushed, got.load(), lost, d.torn_reads(), dup.load(), oob.load());

  const bool corrupted =
      d.torn_reads() > 0 || dup.load() > 0 || oob.load() > 0 || lost != 0;
  if (corrupted) {
    std::printf("value-check FAILED -- the relaxed deque corrupted data "
                "(the bug reproduced on this hardware)\n");
    return 1;
  }
  std::printf("value-check passed on this run -- casual testing does not catch "
              "this. Build with -fsanitize=thread (see `make buggy-demo`).\n");
  return 0;
}
