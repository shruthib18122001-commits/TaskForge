// chase_lev_deque.hpp
// =============================================================================
// A lock-free work-stealing deque (Chase & Lev, SPAA 2005) with the exact C11
// memory orderings from:
//
//   Nhat Minh Lê, Antoniu Pop, Albert Cohen, Francesco Zappa Nardelli.
//   "Correct and Efficient Work-Stealing for Weak Memory Models."
//   PPoPP 2013.  (Referred to below as "LPCN".)
//
// LPCN is the reference because it *proves* the ordering choices against a
// formal C11 model and the ARM/Power memory models -- which is precisely the
// bar this project is held to. Every non-relaxed operation below carries a
// comment naming what it synchronizes with and why a weaker order breaks. The
// prose version of the same argument, with the failing interleavings drawn out,
// is in DESIGN.md.
//
// -----------------------------------------------------------------------------
// Ownership / concurrency contract
// -----------------------------------------------------------------------------
//   * Exactly ONE thread -- "the owner" -- may call push() and pop().
//   * ANY number of other threads -- "thieves" -- may call steal() concurrently
//     with each other and with the owner.
//   * The owner never calls steal() on its own deque; it uses pop().
//
// Violating the single-owner rule (e.g. an external producer calling push())
// is undefined behaviour for this data structure. The thread pool enforces it
// with a separate injection queue -- see thread_pool.hpp.
//
// -----------------------------------------------------------------------------
// Index model
// -----------------------------------------------------------------------------
// `top_` and `bottom_` are 64-bit signed counters, both starting at 0, only
// ever incremented (never wrapped in any realistic runtime -- see "ABA" below).
//
//     bottom_ - top_   == number of elements currently in the deque
//     owner pushes/pops at the `bottom_` end   (LIFO for the owner)
//     thieves steal at the `top_` end          (FIFO w.r.t. the owner)
//
// The backing store is a power-of-two circular buffer ("Ring"); index i maps to
// slot `i & mask`. When it fills, the owner allocates a Ring of twice the size,
// copies the live range [top_, bottom_), and publishes it. Old Rings are kept
// on an owner-private retired list and freed in the destructor (see
// "Memory reclamation").
// =============================================================================

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
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
    // Plain store: no other thread exists yet (the deque is not published).
    ring_.store(r, std::memory_order_relaxed);
    owned_rings_.emplace_back(r);
  }

  ~ChaseLevDeque() {
    // All threads that could touch this deque must already be joined -- that is
    // the pool's shutdown contract. So this is single-threaded cleanup.
    // owned_rings_ holds every Ring ever allocated (current + retired); the
    // unique_ptrs free them here.
  }

  ChaseLevDeque(const ChaseLevDeque&) = delete;
  ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;
  ChaseLevDeque(ChaseLevDeque&&) = delete;
  ChaseLevDeque& operator=(ChaseLevDeque&&) = delete;

  // ------------------------------------------------------------------ push()
  // OWNER ONLY. Never fails: grows the ring if necessary.
  void push(T x) {
    // bottom_: the owner is its only writer, so a relaxed load reads our own
    // last value -- no synchronization needed.
    const std::int64_t b = bottom_.load(std::memory_order_relaxed);

    // top_: ACQUIRE. Thieves publish their progress by CAS-ing top_ with
    // release semantics (see steal()). We must observe a sufficiently recent
    // top_ here so that (a) the fullness test below doesn't under-count live
    // elements, and (b) if we grow, the copy loop's lower bound `t` doesn't
    // include slots thieves have already consumed. A relaxed load could return
    // a stale-small top_, making us copy already-stolen slots -- harmless to
    // correctness of *values* (thieves only read slots, never write them) but
    // it is the order LPCN proves necessary for the resize argument, so we
    // match it.
    const std::int64_t t = top_.load(std::memory_order_acquire);

    // ring_: owner is its only writer (only push()/grow change it) -> relaxed.
    Ring* r = ring_.load(std::memory_order_relaxed);

    if (b - t > r->capacity - 1) {
      // b - t is the current element count. This condition is `count >= cap`,
      // i.e. exactly full: the slot for the new element, (b & mask), aliases
      // the slot still holding element (b - cap), which top_ has not yet passed.
      // Grow (doubling), then continue with the new ring. (LPCN writes the same
      // test as `> size - 1`.)
      r = grow(r, t, b);
    }

    // Write the element with a RELAXED store into the slot...
    r->put(b, x);

    // ...then a RELEASE FENCE, then the RELAXED publish of bottom_.
    //
    // This fence is the producer half of the release/acquire pair that hands
    // the element to a thief. It orders the slot write (above) *before* the
    // bottom_ store (below) in a way a thief can rely on: a thief that reads
    // the new bottom_ with an acquire load (steal()) and then reads the slot
    // is guaranteed to see this element, not whatever the slot held before.
    //
    // Why a standalone fence and not a release store on bottom_ itself? Either
    // works on paper; LPCN uses the fence + relaxed store and it is marginally
    // cheaper on ARM (one `dmb ish` vs. a `stlr` whose ordering is slightly
    // stronger than needed). Stage 9's buggy variant deletes exactly this
    // fence -- on x86 (TSO: stores don't reorder with stores) nothing breaks;
    // on ARM a thief can see bottom_ advance before the slot write lands and
    // steal a stale value.
    std::atomic_thread_fence(std::memory_order_release);
    bottom_.store(b + 1, std::memory_order_relaxed);
  }

  // ------------------------------------------------------------------- pop()
  // OWNER ONLY. LIFO: returns the most recently pushed item, or nullopt.
  std::optional<T> pop() {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    Ring* r = ring_.load(std::memory_order_relaxed);

    // Tentatively claim slot b by publishing the decremented bottom_. A thief
    // that now runs steal() will compute a smaller (b - t) and back off from
    // the last element -- unless it already committed, which the fence + CAS
    // below resolve.
    bottom_.store(b, std::memory_order_relaxed);

    // SEQ_CST FENCE -- the crux of the algorithm.
    //
    // The owner here does:   store(bottom_)   then   load(top_).
    // A thief in steal() does: store(top_) [via CAS] then load(bottom_).
    // Both sequences are store-then-load to *different* locations. That is the
    // one reordering even x86-TSO permits (StoreLoad), and ARM permits it
    // freely. Without a barrier, the owner could read a stale top_ and the
    // thief a stale bottom_, and BOTH conclude the last element is theirs.
    //
    // A seq_cst fence on each side forces both fences into one global total
    // order S. Whichever fence is second in S, the thread executing it sees
    // the other thread's store. So at most one of {owner, thief} proceeds to
    // take the contested element; the other sees the updated counter and backs
    // off. This is the classic Dekker / StoreLoad idiom. DESIGN.md walks the
    // losing interleaving step by step.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // top_: relaxed load is fine *here* -- the seq_cst fence above already
    // provides the ordering we need against the thief's CAS; acquire would add
    // nothing.
    std::int64_t t = top_.load(std::memory_order_relaxed);

    std::optional<T> result;

    if (t <= b) {
      // Non-empty.
      T x = r->get(b);  // relaxed: this slot was written by us (the owner)
      if (t == b) {
        // Exactly one element, and a thief may be going for the same one.
        // Resolve with a CAS on top_.
        //   success ordering seq_cst: this CAS must sit in the SAME total order
        //     S as the fence above and as the thief's steal() CAS, or the
        //     Dekker argument doesn't close. acq_rel is NOT enough here (it
        //     wouldn't be ordered against our own seq_cst fence on ARM).
        //   failure ordering relaxed: on failure a thief won; we took nothing,
        //     we just need the updated `t`, and we re-establish state below.
        if (!top_.compare_exchange_strong(t, t + 1,
                                          std::memory_order_seq_cst,
                                          std::memory_order_relaxed)) {
          // Thief got it.
          result.reset();
        } else {
          result = x;
        }
        // Whether we won or lost, the deque is now empty; restore the canonical
        // empty representation bottom_ == top_.
        bottom_.store(b + 1, std::memory_order_relaxed);
      } else {
        // t < b: at least one element below the contested top; no thief can be
        // racing us for slot b. Take it outright.
        result = x;
      }
    } else {
      // t > b: empty. Undo the tentative decrement.
      result.reset();
      bottom_.store(b + 1, std::memory_order_relaxed);
    }
    return result;
  }

  // ----------------------------------------------------------------- steal()
  // ANY thief thread. FIFO w.r.t. the owner. Three outcomes -- see StealStatus.
  StealStatus steal(T& out) {
    // top_: ACQUIRE. Pairs with the releasing CAS of whichever thief most
    // recently advanced top_ (and, transitively, with the owner's pop() CAS).
    // Ensures our subsequent view of ring_ / slots is at least as recent as
    // that thief's.
    std::int64_t t = top_.load(std::memory_order_acquire);

    // SEQ_CST FENCE -- thief half of the Dekker pair with pop()'s fence.
    // Orders our (upcoming) top_ CAS store against our bottom_ load below,
    // within the global total order S. See the long comment in pop().
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // bottom_: ACQUIRE. Pairs with the RELEASE FENCE in push(). This is the
    // consumer half of the element handoff: having synchronized here, when we
    // read the slot below we are guaranteed to see the value push() stored,
    // not a stale slot. Downgrading this to relaxed is the mirror image of
    // Stage 9's bug.
    const std::int64_t b = bottom_.load(std::memory_order_acquire);

    if (t >= b) {
      return StealStatus::kEmpty;  // observably empty
    }

    // Non-empty. Load the current ring with ACQUIRE, pairing with the
    // ring_.store(release) in grow(). If we raced a grow and used a stale ring,
    // slot `t & old_mask` might not hold element t after the copy -> we'd
    // return the wrong value. (LPCN uses memory_order_consume here; consume is
    // unimplemented-as-specified by every mainstream compiler and compiled as
    // acquire anyway, so we ask for acquire explicitly. The dependency it is
    // meant to protect is ring -> ring->slot[t].)
    Ring* r = ring_.load(std::memory_order_acquire);
    T x = r->get(t);  // relaxed slot load; ordering supplied by the acquires above

    // Commit by advancing top_ past t.
    //   success seq_cst: same reason as pop()'s CAS -- must lie in S.
    //   failure relaxed: another thief (or the owner) moved top_; `t` is
    //     refreshed but we don't retry in-place, we report kAbort and let the
    //     caller pick another victim. Retrying here risks livelock under heavy
    //     contention; spreading the retry across victims is strictly better.
    if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                      std::memory_order_relaxed)) {
      return StealStatus::kAbort;
    }

    out = x;
    return StealStatus::kSuccess;
  }

  // Convenience wrapper: nullopt covers both kEmpty and kAbort (the caller that
  // needs to tell them apart -- e.g. steal-rate instrumentation -- uses the
  // StealStatus overload).
  std::optional<T> steal() {
    T tmp{};
    if (steal(tmp) == StealStatus::kSuccess) return tmp;
    return std::nullopt;
  }

  // --------------------------------------------------------------- observers
  // Approximate: correct only if no one is mutating concurrently. For
  // instrumentation and tests, never for control flow on the hot path.
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
  // How many times the ring has doubled -- a cost signal for benchmarks.
  std::size_t grow_count() const { return owned_rings_.size() - 1; }

 private:
  struct Ring {
    const std::int64_t capacity;
    const std::int64_t mask;
    std::unique_ptr<std::atomic<T>[]> slots;

    explicit Ring(std::int64_t cap)
        : capacity(cap), mask(cap - 1), slots(new std::atomic<T>[static_cast<std::size_t>(cap)]) {
      // value-initialised: every slot starts as T() (nullptr / 0).
    }

    // All slot access is relaxed; ordering is imposed by the fences/among
    // top_/bottom_ in push/pop/steal, never here.
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
    // that sees this new pointer is guaranteed to also see the copied slot
    // contents above.
    ring_.store(nr, std::memory_order_release);

    // Retire the old ring. We do NOT free it now: a thief may still be inside
    // steal() holding `old_ring` and about to read a slot from it. We keep
    // every ring alive until the deque is destroyed (all threads joined).
    // owned_rings_ is touched only by the owner and only on this cold path, so
    // the push_back's potential allocation never hits the steady-state hot
    // path. Cost: O(log n) leaked buffers for the deque's lifetime, which for
    // a scheduler (deques live as long as the pool) is nothing. A
    // general-purpose deque would need hazard pointers or epoch-based
    // reclamation instead; that trade is discussed in DESIGN.md.
    owned_rings_.emplace_back(nr);
    return nr;
  }

  static std::int64_t round_up_pow2(std::int64_t v) {
    std::int64_t p = 1;
    while (p < v) p <<= 1;
    return p;
  }

  // Hot atomics, each on its own cache line: the owner writes bottom_ and reads
  // top_; thieves write top_ and read bottom_. Sharing a line would put those
  // on the same coherence unit and serialize owner vs. thieves for no reason.
  alignas(TF_CACHELINE) std::atomic<std::int64_t> top_;
  alignas(TF_CACHELINE) std::atomic<std::int64_t> bottom_;
  alignas(TF_CACHELINE) std::atomic<Ring*> ring_;

  // Cold, owner-only: every Ring ever allocated, freed at destruction.
  std::vector<std::unique_ptr<Ring>> owned_rings_;

  static_assert(std::atomic<std::int64_t>::is_always_lock_free,
                "need lock-free 64-bit atomics");
};

}  // namespace taskforge
