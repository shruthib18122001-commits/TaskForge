// chase_lev_deque_relaxed.hpp
// =============================================================================
// Stage 9: THE DELIBERATELY WRONG DEQUE. Do not use this for anything.
//
// This is chase_lev_deque.hpp with the element-publication synchronisation
// removed, and the element widened so the missing edge is easy to observe.
//
// THE BUG (one line): push() advances bottom_ with a RELAXED store instead of a
// RELEASE store.
//
// Why that is fatal: in the correct deque the release store on bottom_ pairs
// with steal()'s acquire (seq_cst) load of bottom_. That happens-before edge is
// the ONLY thing that lets the element slots be touched with ordinary,
// unsynchronised memory operations -- everything sequenced before the release
// (the slot write) is guaranteed visible to everything sequenced after the
// acquire (the slot read). Delete the release and that guarantee is gone.
//
// To make the consequence visible rather than merely latent, each slot is a
// two-word cell { v, v_copy } written as two plain stores and read as two plain
// loads. Two ways this shows up:
//   - the thief straddles the publish: v fresh, v_copy stale  -> v != v_copy
//     (counted in torn_reads())
//   - the thief reads the whole cell before push's stores land: it gets the
//     PREVIOUS element back  -> a duplicate value at the caller
//
// Observed:
//   * single-threaded / light tests ........... pass
//   * -O2 stress, x86-64 (TSO) ................ passes: x86 does not reorder the
//                                              stores; word load/store don't tear
//   * -O2 stress, Apple M5 ................... FAILS every run: tens of thousands
//                                              of duplicate values per few
//                                              million ops (stale slot re-read)
//   * ThreadSanitizer ........................ reports a data race on the cell
//                                              (plain write in push races plain
//                                              read in steal; no happens-before)
//
// The lesson: for lock-free code the memory model is the specification; "it
// passed on my machine" proves nothing.
// =============================================================================

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "taskforge/chase_lev_deque.hpp"  // TF_CACHELINE, StealStatus

namespace taskforge {

class ChaseLevDequeRelaxed {
 public:
  explicit ChaseLevDequeRelaxed(std::int64_t initial_capacity = 1024)
      : top_(0), bottom_(0), ring_(nullptr) {
    std::int64_t cap = round_up_pow2(initial_capacity < 64 ? 64 : initial_capacity);
    Ring* r = new Ring(cap);
    ring_.store(r, std::memory_order_relaxed);
    owned_rings_.emplace_back(r);
  }
  ~ChaseLevDequeRelaxed() = default;

  ChaseLevDequeRelaxed(const ChaseLevDequeRelaxed&) = delete;
  ChaseLevDequeRelaxed& operator=(const ChaseLevDequeRelaxed&) = delete;

  // Count of times steal()/pop() returned a cell whose two words disagreed --
  // i.e. the caller observed a half-published element. Always 0 for a correct
  // deque. Read after the run.
  std::int64_t torn_reads() const { return torn_.load(std::memory_order_relaxed); }

  // OWNER ONLY.
  void push(std::uint64_t x) {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed);
    const std::int64_t t = top_.load(std::memory_order_acquire);
    Ring* r = ring_.load(std::memory_order_relaxed);
    if (b - t > r->capacity - 1) r = grow(r, t, b);

    Cell& c = r->cells[static_cast<std::size_t>(b & r->mask)];
    c.v = x;        // plain store 1
    c.v_copy = x;   // plain store 2  (a correct deque publishes both atomically
                    //                 w.r.t. the thief, via the release below)

    // THE BUG: relaxed instead of release. A thief that observes b+1 is no
    // longer guaranteed to observe the two stores above.
    bottom_.store(b + 1, std::memory_order_relaxed);
  }

  // OWNER ONLY.
  std::optional<std::uint64_t> pop() {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    Ring* r = ring_.load(std::memory_order_relaxed);
    bottom_.store(b, std::memory_order_seq_cst);
    std::int64_t t = top_.load(std::memory_order_seq_cst);
    std::optional<std::uint64_t> result;
    if (t <= b) {
      const Cell c = r->cells[static_cast<std::size_t>(b & r->mask)];  // owner is sole writer -> fine here
      if (t == b) {
        if (top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                         std::memory_order_relaxed)) {
          result = value_of(c);
        }
        bottom_.store(b + 1, std::memory_order_relaxed);
      } else {
        result = value_of(c);
      }
    } else {
      bottom_.store(b + 1, std::memory_order_relaxed);
    }
    return result;
  }

  // ANY thief.
  StealStatus steal(std::uint64_t& out) {
    std::int64_t t = top_.load(std::memory_order_acquire);
    const std::int64_t b = bottom_.load(std::memory_order_seq_cst);
    if (t >= b) return StealStatus::kEmpty;

    Ring* r = ring_.load(std::memory_order_acquire);
    // Two plain loads. With the missing release these race push()'s two plain
    // stores, and can straddle the publish: v fresh, v_copy stale (or torn).
    const std::uint64_t v = r->cells[static_cast<std::size_t>(t & r->mask)].v;
    const std::uint64_t vc = r->cells[static_cast<std::size_t>(t & r->mask)].v_copy;

    if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                      std::memory_order_relaxed)) {
      return StealStatus::kAbort;
    }
    if (v != vc) torn_.fetch_add(1, std::memory_order_relaxed);
    out = v;
    return StealStatus::kSuccess;
  }

  std::optional<std::uint64_t> steal() {
    std::uint64_t tmp = 0;
    if (steal(tmp) == StealStatus::kSuccess) return tmp;
    return std::nullopt;
  }

  std::int64_t size() const {
    const std::int64_t n = bottom_.load(std::memory_order_relaxed) -
                           top_.load(std::memory_order_relaxed);
    return n > 0 ? n : 0;
  }
  bool empty() const { return size() == 0; }

 private:
  struct Cell {
    std::uint64_t v = 0;
    std::uint64_t v_copy = 0;
  };
  struct Ring {
    const std::int64_t capacity;
    const std::int64_t mask;
    std::unique_ptr<Cell[]> cells;
    explicit Ring(std::int64_t cap)
        : capacity(cap), mask(cap - 1), cells(new Cell[static_cast<std::size_t>(cap)]()) {}
  };

  std::uint64_t value_of(const Cell& c) {
    if (c.v != c.v_copy) torn_.fetch_add(1, std::memory_order_relaxed);
    return c.v;
  }

  Ring* grow(Ring* old_ring, std::int64_t t, std::int64_t b) {
    Ring* nr = new Ring(old_ring->capacity * 2);
    for (std::int64_t i = t; i < b; ++i)
      nr->cells[static_cast<std::size_t>(i & nr->mask)] =
          old_ring->cells[static_cast<std::size_t>(i & old_ring->mask)];
    ring_.store(nr, std::memory_order_release);
    owned_rings_.emplace_back(nr);
    return nr;
  }

  static std::int64_t round_up_pow2(std::int64_t v) {
    std::int64_t p = 1;
    while (p < v) p <<= 1;
    return p;
  }

  alignas(TF_CACHELINE) std::atomic<std::int64_t> top_;
  alignas(TF_CACHELINE) std::atomic<std::int64_t> bottom_;
  alignas(TF_CACHELINE) std::atomic<Ring*> ring_;
  alignas(TF_CACHELINE) std::atomic<std::int64_t> torn_{0};
  std::vector<std::unique_ptr<Ring>> owned_rings_;
};

}  // namespace taskforge
