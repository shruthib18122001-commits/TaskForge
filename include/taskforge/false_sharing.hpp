// false_sharing.hpp
// =============================================================================
// Stage 8, done on purpose in two steps.
//
// thread_pool.hpp updates a per-worker counter (tasks_executed, steal counts,
// ...) on essentially every task. If those per-worker blocks share a cache
// line, each update invalidates the line in every other core's cache and the
// line ping-pongs -- "false sharing": no logical contention, pure coherence
// traffic. The pool avoids it with `alignas(TF_CACHELINE)` on WorkerCounters.
//
// This header isolates exactly that update so the effect can be measured with
// and without the alignment. Each thread hammers its own counter in a tight
// loop; the only thing that changes between the two runs is whether adjacent
// counters share a line.
//
//   PackedSlot   : 8-byte atomic, no alignment  -> N of them pack into
//                  64/PackedSlot cache lines, so up to 8 threads share one line
//   AlignedSlot  : alignas(64)                  -> one line each, never shared
//
// run_false_sharing() returns wall time for N threads x iters increments.
// bench_main wires it to `--bench false-sharing` (throughput vs thread count,
// both layouts); scripts/run_perf.sh adds cache-miss counts on Linux.
// =============================================================================

#pragma once

#include "taskforge/chase_lev_deque.hpp"  // TF_CACHELINE

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace taskforge {

struct PackedSlot {
  std::atomic<std::uint64_t> v{0};
};

struct alignas(TF_CACHELINE) AlignedSlot {
  std::atomic<std::uint64_t> v{0};
};

enum class FsLayout { kPacked, kAligned };

struct FsResult {
  double seconds = 0.0;
  std::uint64_t checksum = 0;  // == nthreads * iters if the counting is correct
};

// N threads, each doing `iters` relaxed fetch_add on its OWN slot. Bars the
// threads on an atomic gate so they hammer concurrently. Returns wall seconds
// (and, via the *_checked wrapper, the summed counter value).
template <class Slot>
FsResult run_false_sharing_checked_impl(unsigned nthreads, std::uint64_t iters) {
  // Contiguous array: with PackedSlot this is where the sharing happens.
  std::unique_ptr<Slot[]> slots(new Slot[nthreads]);

  std::atomic<unsigned> ready{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> ts;
  ts.reserve(nthreads);

  for (unsigned t = 0; t < nthreads; ++t) {
    ts.emplace_back([&, t] {
      ready.fetch_add(1, std::memory_order_acq_rel);
      while (!go.load(std::memory_order_acquire)) {
      }
      std::atomic<std::uint64_t>& mine = slots[t].v;
      for (std::uint64_t i = 0; i < iters; ++i)
        mine.fetch_add(1, std::memory_order_relaxed);
    });
  }

  while (ready.load(std::memory_order_acquire) != nthreads) {
  }
  const auto t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  for (auto& th : ts) th.join();
  const auto t1 = std::chrono::steady_clock::now();

  std::uint64_t sum = 0;
  for (unsigned t = 0; t < nthreads; ++t)
    sum += slots[t].v.load(std::memory_order_relaxed);

  return FsResult{std::chrono::duration<double>(t1 - t0).count(), sum};
}

inline FsResult run_false_sharing_checked(FsLayout layout, unsigned nthreads,
                                          std::uint64_t iters) {
  return layout == FsLayout::kPacked
             ? run_false_sharing_checked_impl<PackedSlot>(nthreads, iters)
             : run_false_sharing_checked_impl<AlignedSlot>(nthreads, iters);
}

inline double run_false_sharing(FsLayout layout, unsigned nthreads,
                                std::uint64_t iters) {
  return run_false_sharing_checked(layout, nthreads, iters).seconds;
}

}  // namespace taskforge
