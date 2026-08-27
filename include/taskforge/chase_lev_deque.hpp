// chase_lev_deque.hpp
// =============================================================================
// A lock-free work-stealing deque (Chase & Lev, SPAA 2005).
//
// The algorithm is Chase-Lev; the memory-ordering treatment follows
//
//   Nhat Minh Le, Antoniu Pop, Albert Cohen, Francesco Zappa Nardelli.
//   "Correct and Efficient Work-Stealing for Weak Memory Models." PPoPP 2013.
//   (Referred to below as "LPCN".)
//
// with ONE deliberate departure: LPCN publishes the element in push() with a
// standalone `atomic_thread_fence(release)`, and separates the owner's and
// thief's last-element accesses with `atomic_thread_fence(seq_cst)`. This file
// instead carries those orderings on the atomic operations themselves -- a
// release store on `bottom_` in push(), and seq_cst on the `bottom_`/`top_`
// accesses that form the Dekker pair between pop() and steal().
//
// Why the departure: ThreadSanitizer does not reliably model standalone
// `std::atomic_thread_fence` (documented limitation; on arm64 it caused TSan to
// hang under this project's mixed owner/thief pool workload -- see DESIGN.md
// "what didn't work"). The per-operation formulation is provably equivalent
// (the argument, below and in DESIGN.md, is a total order over all seq_cst
// operations), is what crossbeam-deque / Taskflow-style implementations use in
// practice, and is TSan-clean. It is also, arguably, easier to reason about:
// "these operations are seq_cst, hence totally ordered, and here is the
// interleaving argument over that order."
//
// -----------------------------------------------------------------------------
// Ownership / concurrency contract
// -----------------------------------------------------------------------------
//   * Exactly ONE thread -- "the owner" -- may call push() and pop().
//   * ANY number of other threads -- "thieves" -- may call steal() concurrently
//     with each other and with the owner.
//   * The owner never calls steal() on its own deque; it uses pop().
// Violating single-owner is undefined behaviour for this structure. The thread
// pool enforces it with a separate injection queue (see thread_pool.hpp).
//
// -----------------------------------------------------------------------------
// Index model
// -----------------------------------------------------------------------------
// `top_` and `bottom_` are 64-bit signed counters, both starting at 0, only
// ever incremented (never wrapped in any realistic runtime -- see "ABA").
//
//     bottom_ - top_  == number of elements currently in the deque
//     owner pushes / pops at the `bottom_` end   (LIFO for the owner)
//     thieves steal at the `top_` end            (FIFO w.r.t. the owner)
//
// Storage is a power-of-two circular buffer ("Ring"); index i -> slot i & mask.
// When it fills, the owner allocates a Ring of double the size, copies the live
// range [top_, bottom_), and publishes it with a release store. Old Rings are
// kept on an owner-private retired list and freed in the destructor (see
// "Memory reclamation").
// =============================================================================

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

namespace taskforge {

// Coherence granularity we align hot atomics to. 64 on mainstream x86-64.
// Apple Silicon's L2 works in 128-byte pairs for coherence; Stage 8's
// false-sharing experiment revisits whether 128 changes anything measurable.
#ifndef TF_CACHELINE
#define TF_CACHELINE 64
#endif

enum class StealStatus {
  kSuccess,  // got an item
  kEmpty,    // deque was observably empty
  kAbort     // lost the CAS race for the top item; caller should retry elsewhere
};

template <class T>
class ChaseLevDeque {
  static_assert(std::is_trivially_copyable<T>::value,
                "ChaseLevDeque<T> stores T in std::atomic<T>; T must be "
                "trivially copyable (use a pointer or a small POD).");
  static_assert(std::atomic<T>::is_always_lock_free,
                "ChaseLevDeque<T> requires std::atomic<T> to be lock-free; "
                "T is too large. Store a pointer instead.");

 public:
  // initial_capacity is rounded up to a power of two, floor 64.
  explicit ChaseLevDeque(std::int64_t initial_capacity = 1024)
      : top_(0), bottom_(0), ring_(nullptr) {
    std::int64_t cap = round_up_pow2(initial_capacity < 64 ? 64 : initial_capacity);
    Ring* r = new Ring(cap);
    ring_.store(r, std::memory_order_relaxed);  // not published yet: no other thread
    owned_rings_.emplace_back(r);
  }

  ~ChaseLevDeque() = default;  // owned_rings_ frees every Ring; all threads joined by now

  ChaseLevDeque(const ChaseLevDeque&) = delete;
  ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;
  ChaseLevDeque(ChaseLevDeque&&) = delete;
  ChaseLevDeque& operator=(ChaseLevDeque&&) = delete;

  // ------------------------------------------------------------------ push()
  // OWNER ONLY. Never fails: grows the ring if necessary.
  void push(T x) {
    // bottom_: the owner is its only writer, so a relaxed load reads our own
    // last value.
    const std::int64_t b = bottom_.load(std::memory_order_relaxed);

    // top_: ACQUIRE. Thieves publish progress by CAS-ing top_ with release
    // semantics (see steal()). Observing a recent top_ here keeps the fullness
    // test from under-counting and keeps grow()'s copy lower-bound honest.
    // (LPCN proves this load must be at least acquire.)
    const std::int64_t t = top_.load(std::memory_order_acquire);

    // ring_: owner is its only writer -> relaxed.
    Ring* r = ring_.load(std::memory_order_relaxed);

    if (b - t > r->capacity - 1) {
      // b - t is the current element count; this condition is `count >= cap`,
      // i.e. exactly full -- the slot for the new element, (b & mask), aliases
      // the slot still holding element (b - cap), which top_ has not passed.
      r = grow(r, t, b);
    }

    // Write the element with a RELAXED slot store, then publish it by advancing
    // bottom_ with a RELEASE store.
    //
    // This release store is the producer half of the release/acquire pair that
    // hands the element to a thief: a thief that reads this new bottom_ value
    // with an acquire load (steal()) synchronizes-with this store, so the slot
    // write sequenced before it is visible to the slot read the thief does
    // after. Replacing `release` with `relaxed` here is exactly Stage 9's bug:
    // on x86 (TSO: no store-store reorder) nothing breaks; on ARM a thief can
    // see bottom_ advance before the slot write lands and steal a stale value.
    r->put(b, x);
    bottom_.store(b + 1, std::memory_order_release);
  }

  // ------------------------------------------------------------------- pop()
  // OWNER ONLY. LIFO: returns the most recently pushed item, or nullopt.
  std::optional<T> pop() {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    Ring* r = ring_.load(std::memory_order_relaxed);

    // Tentatively claim slot b by publishing the decremented bottom_ with a
    // SEQ_CST store. Together with the seq_cst load of top_ just below, and
    // steal()'s seq_cst load of bottom_ and seq_cst CAS of top_, these form a
    // set of operations with a single global total order S.
    //
    // The Dekker argument: the owner does store(bottom_) then load(top_); a
    // thief does load(bottom_) then CAS(top_). All four are seq_cst, so in S
    // either the owner's store precedes the thief's load (thief sees the
    // decremented bottom_ and backs off the last element) or it does not (then
    // the owner's load sees the thief's CAS, or the two CASes on top_ race and
    // exactly one wins). Either way at most one of {owner, thief} takes the
    // contested element. DESIGN.md walks the losing interleaving that a weaker
    // order would allow.
    bottom_.store(b, std::memory_order_seq_cst);
    std::int64_t t = top_.load(std::memory_order_seq_cst);

    std::optional<T> result;

    if (t <= b) {
      T x = r->get(b);  // relaxed: this slot was written by us, the owner
      if (t == b) {
        // Exactly one element; a thief may be going for the same one.
        //   success seq_cst: this CAS is part of S (see above).
        //   failure relaxed: on failure a thief won; we took nothing and only
        //     need the refreshed t before restoring state.
        if (top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                         std::memory_order_relaxed)) {
          result = x;
        }
        // Won or lost, the deque is now empty: restore bottom_ == top_.
        bottom_.store(b + 1, std::memory_order_relaxed);
      } else {
        // t < b: at least one element sits below the contested top; no thief
        // can be racing us for slot b. Take it outright, bottom_ stays at b.
        result = x;
      }
    } else {
      // t > b: empty. Undo the tentative decrement.
      bottom_.store(b + 1, std::memory_order_relaxed);
    }
    return result;
  }

  // ----------------------------------------------------------------- steal()
  // ANY thief thread. FIFO w.r.t. the owner. Three outcomes -- see StealStatus.
  StealStatus steal(T& out) {
    // top_: ACQUIRE. Pairs with the releasing CAS of whichever thief last
    // advanced top_. A stale value here only costs a failed CAS -> kAbort.
    std::int64_t t = top_.load(std::memory_order_acquire);

    // bottom_: SEQ_CST load. This and the seq_cst CAS of top_ below are the
    // thief's contributions to the total order S described in pop(). Reading a
    // sufficiently recent bottom_ also means that, having found the deque
    // non-empty, the slot read below sees the element push() published with its
    // release store (this load synchronizes-with that store).
    const std::int64_t b = bottom_.load(std::memory_order_seq_cst);

    if (t >= b) {
      return StealStatus::kEmpty;  // observably empty
    }

    // Non-empty. Load the ring with ACQUIRE, pairing with grow()'s release
    // store. A stale ring would index element t into the wrong slot after a
    // resize. (LPCN uses memory_order_consume here; consume is compiled as
    // acquire by every mainstream compiler, so we ask for acquire directly.)
    Ring* r = ring_.load(std::memory_order_acquire);
    T x = r->get(t);  // relaxed slot load; ordering supplied by the loads above

    // Commit by advancing top_ past t.
    //   success seq_cst: part of S (Dekker, see pop()).
    //   failure relaxed: someone else moved top_; report kAbort and let the
    //     caller pick another victim -- retrying in place risks livelock under
    //     contention, spreading the retry across victims does not.
    if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                      std::memory_order_relaxed)) {
      return StealStatus::kAbort;
    }
    out = x;
    return StealStatus::kSuccess;
  }

  // Convenience wrapper: nullopt covers both kEmpty and kAbort. Callers that
  // must tell them apart (steal-rate instrumentation) use the StealStatus form.
  std::optional<T> steal() {
    T tmp{};
    if (steal(tmp) == StealStatus::kSuccess) return tmp;
    return std::nullopt;
  }

  // --------------------------------------------------------------- observers
  // Approximate unless the caller knows no one is mutating concurrently. For
  // instrumentation and tests, never for hot-path control flow.
  std::int64_t size() const {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed);
    const std::int64_t t = top_.load(std::memory_order_relaxed);
    const std::int64_t n = b - t;
    return n > 0 ? n : 0;
  }
  bool empty() const { return size() == 0; }

  std::int64_t capacity() const {
    return ring_.load(std::memory_order_relaxed)->capacity;
  }
  std::size_t grow_count() const { return owned_rings_.size() - 1; }

 private:
  struct Ring {
    const std::int64_t capacity;
    const std::int64_t mask;
    std::unique_ptr<std::atomic<T>[]> slots;

    explicit Ring(std::int64_t cap)
        : capacity(cap),
          mask(cap - 1),
          slots(new std::atomic<T>[static_cast<std::size_t>(cap)]) {
      // value-initialised: every slot starts as T() (nullptr / 0).
    }

    T get(std::int64_t i) const {
      return slots[static_cast<std::size_t>(i & mask)].load(std::memory_order_relaxed);
    }
    void put(std::int64_t i, T v) {
      slots[static_cast<std::size_t>(i & mask)].store(v, std::memory_order_relaxed);
    }
  };

  // OWNER ONLY. Allocate a 2x ring, copy the live range [t, b), publish it.
  Ring* grow(Ring* old_ring, std::int64_t t, std::int64_t b) {
    Ring* nr = new Ring(old_ring->capacity * 2);
    for (std::int64_t i = t; i < b; ++i) {
      nr->put(i, old_ring->get(i));
    }
    // RELEASE store: pairs with the acquire load of ring_ in steal(). A thief
    // that sees this pointer also sees the copied slot contents above.
    ring_.store(nr, std::memory_order_release);

    // Retire, do NOT free: a thief may still hold `old_ring` inside steal().
    // Every ring is kept alive until the deque is destroyed (all threads
    // joined). owned_rings_ is touched only by the owner and only on this cold
    // path, so its push_back never hits the steady-state hot path. Cost: a few
    // stale buffers for the deque's lifetime -- negligible for a scheduler,
    // where deques live as long as the pool. A general-purpose deque would use
    // hazard pointers or epoch-based reclamation; that trade is in DESIGN.md.
    owned_rings_.emplace_back(nr);
    return nr;
  }

  static std::int64_t round_up_pow2(std::int64_t v) {
    std::int64_t p = 1;
    while (p < v) p <<= 1;
    return p;
  }

  // Hot atomics, each on its own cache line: the owner writes bottom_ and reads
  // top_; thieves write top_ and read bottom_. Sharing a line would serialize
  // owner and thieves on the same coherence unit for no reason.
  alignas(TF_CACHELINE) std::atomic<std::int64_t> top_;
  alignas(TF_CACHELINE) std::atomic<std::int64_t> bottom_;
  alignas(TF_CACHELINE) std::atomic<Ring*> ring_;

  // Cold, owner-only: every Ring ever allocated, freed at destruction.
  std::vector<std::unique_ptr<Ring>> owned_rings_;
};

}  // namespace taskforge
