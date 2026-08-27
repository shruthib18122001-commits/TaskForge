# DESIGN.md

The reference for every synchronization decision in TaskForge. If a line uses
`acquire` instead of `seq_cst`, this file says why. Written to study from.

Contents:

1. [Model and invariants](#1-model-and-invariants)
2. [The Chase–Lev deque, operation by operation](#2-the-chaselev-deque-operation-by-operation)
3. [The Dekker argument for the last element](#3-the-dekker-argument-for-the-last-element)
4. [The CAS on `top_`, and ABA](#4-the-cas-on-top_-and-aba)
5. [Standalone fences vs per-operation ordering](#5-standalone-fences-vs-per-operation-ordering)
6. [x86-TSO vs ARM: what each ordering costs](#6-x86-tso-vs-arm-what-each-ordering-costs)
7. [Memory reclamation of grown buffers](#7-memory-reclamation-of-grown-buffers)
8. [The thread pool](#8-the-thread-pool)
9. [The DAG resolver](#9-the-dag-resolver)
10. [Stage 9: the deliberately wrong ordering](#10-stage-9-the-deliberately-wrong-ordering)
11. [Exception safety](#11-exception-safety)
12. [Clean shutdown](#12-clean-shutdown)

---

## 1. Model and invariants

The deque (`include/taskforge/chase_lev_deque.hpp`) has two 64-bit signed
counters and a power-of-two circular buffer:

```
      top_        thieves take from here (FIFO w.r.t. the owner)
      bottom_     the owner pushes / pops here (LIFO)
      ring_       atomic pointer to the current backing buffer
```

Invariants, maintained at all times a reader could observe a consistent state:

- **`size == bottom_ - top_`.** `> 0` means non-empty, `0` means empty, and a
  transient `< 0` is possible *only* inside `pop()` between its tentative
  `bottom_` decrement and the restore — a concurrent `steal()` that observes it
  computes `t >= b` and reports empty, which is correct.
- **`top_` and `bottom_` only ever increase.** Never decremented below a value
  already published, never wrapped (see §4). This is what kills ABA.
- **Exactly one thread is the owner.** It alone calls `push()` and `pop()`.
  Any number of *thieves* call `steal()`. The owner never steals from its own
  deque. Breaking single-owner is undefined behaviour for the structure; the
  pool enforces it with a separate injection queue (§8).
- **Element slots are `std::atomic<T>` accessed `relaxed`.** All ordering that
  makes a pushed element visible to a thief comes from the `bottom_` /`top_`
  operations, never from the slot accesses themselves.

`T` must be trivially copyable and `std::atomic<T>` must be always-lock-free
(`static_assert`ed). The pool instantiates `ChaseLevDeque<Task*>`.

---

## 2. The Chase–Lev deque, operation by operation

Algorithm: Chase & Lev, *Dynamic Circular Work-Stealing Deque*, SPAA 2005.
Orderings follow Lê, Pop, Cohen, Zappa Nardelli, *Correct and Efficient
Work-Stealing for Weak Memory Models*, PPoPP 2013 (**LPCN**), with the
fence→operation transformation of §5.

### `push(x)` — owner only

| # | operation | order | synchronizes with | why not weaker | why not stronger |
|---|---|---|---|---|---|
| p1 | `b = bottom_.load` | `relaxed` | — (owner is the only writer of `bottom_`) | we only ever read our own last value | acquire/seq_cst would be a barrier for nothing |
| p2 | `t = top_.load` | `acquire` | a thief's releasing CAS on `top_` (s4/k4) | a stale-large `t` is impossible (monotonic); a stale-small `t` would make the fullness test *over*-count and the `grow()` copy range too wide — harmless to values but LPCN proves acquire is required for the resize argument, so we match it | seq_cst adds a StoreLoad barrier the resize logic doesn't need |
| p3 | `r = ring_.load` | `relaxed` | — (owner is the only writer of `ring_`) | same as p1 | — |
| p4 | `r->put(b, x)` | `relaxed` | (published by p5) | the publish edge is p5; the slot store just has to be *sequenced before* it | atomic RMW / release on the slot would be per-element cost for no gain |
| p5 | `bottom_.store(b+1)` | **`release`** | `steal()`'s `bottom_.load` (s2) and `pop()`'s seq_cst load path | **this is the publish.** `relaxed` here is exactly Stage 9's bug: a thief that reads `b+1` would have no happens-before with p4 and could read a stale slot | `seq_cst` would additionally order this store against unrelated seq_cst ops globally — not needed for the handoff, and the owner is on the hot path |

`grow()` (cold path, owner only): allocate a 2× ring, copy `[t, b)`, then
`ring_.store(nr, release)` — pairs with `steal()`'s `ring_.load(acquire)` (s3)
so a thief that sees the new pointer also sees the copied contents.

### `pop()` — owner only

| # | operation | order | synchronizes with | why not weaker | why not stronger |
|---|---|---|---|---|---|
| o1 | `b = bottom_.load - 1` | `relaxed` | — | own last value | — |
| o2 | `r = ring_.load` | `relaxed` | — | own last value | — |
| o3 | `bottom_.store(b)` | **`seq_cst`** | forms the total order **S** with o4, s2, s4 | a `relaxed`/`release` store here + a plain load at o4 permits the StoreLoad reorder that lets owner and thief both take the last element (§3) | — (seq_cst is the minimum that closes §3) |
| o4 | `t = top_.load` | **`seq_cst`** | S | as o3 | — |
| o5 | `r->get(b)` | `relaxed` | — (this slot was written by us, the owner) | owner is the sole writer of the slot | — |
| o6 | `top_.CAS(t → t+1)` (only when `t == b`) | success **`seq_cst`**, failure `relaxed` | S (success); nothing (failure) | with only `acq_rel`, this CAS is not ordered in S against o3/o4 and §3 reopens on ARM | — |
| o7 | `bottom_.store(b+1)` (restore) | `relaxed` | — | after o6 the deque is empty and only the owner writes `bottom_`; no thief needs to see the restore urgently (the next `push`/`pop` re-establishes order) | — |

When `t < b` (not the last element) `pop()` skips o6/o7 and just returns the
slot — no thief can be racing for slot `b` because `top_` is well below it.

### `steal(out)` — any thief

| # | operation | order | synchronizes with | why not weaker | why not stronger |
|---|---|---|---|---|---|
| s1 | `t = top_.load` | `acquire` | the last thief's releasing CAS on `top_` (s4) | a stale `t` only costs a failed CAS → `kAbort`; but we still want to see a coherent successor buffer, so acquire | seq_cst here is not needed — the Dekker-critical thief ops are s2 and s4 |
| s2 | `b = bottom_.load` | **`seq_cst`** | S; and (as an acquire load) `push()`'s `release` store p5 | `relaxed` breaks §3 *and* removes the synchronizes-with with p5, so s5 could read a stale slot | — |
| s3 | `r = ring_.load` | `acquire` | `grow()`'s `ring_.store(release)` | a stale ring indexes element `t` into the wrong slot after a resize → wrong value returned | consume (what LPCN writes) is compiled as acquire everywhere anyway |
| s4 | `x = r->get(t)` | `relaxed` | — | ordering is supplied by s2 (HB with p5) and s3 | — |
| s5 | `top_.CAS(t → t+1)` | success **`seq_cst`**, failure `relaxed` | S (success) | `acq_rel` success is not ordered in S vs o3/o4 → §3 reopens | — |

On CAS failure `steal()` returns `kAbort` (someone else moved `top_`); the pool
tries a different victim rather than retrying in place, which avoids livelock
under contention (see §8, victim selection).

---

## 3. The Dekker argument for the last element

The one hard race: the deque holds exactly one element (`bottom_ == top_ + 1`)
and the owner's `pop()` and a thief's `steal()` both go for it. Without enough
ordering, **both** can conclude it's theirs.

Line up the two sides:

```
owner pop():                         thief steal():
  o3  bottom_.store(b, seq_cst)         s2  b = bottom_.load(seq_cst)
  o4  t = top_.load(seq_cst)            s5  top_.CAS(t→t+1, seq_cst)
```

Each side is *store-then-load to a different location*. That is the one
reordering **x86-TSO still allows** (StoreLoad) and ARM allows freely. If the
owner's o3 and o4 can be observed out of order, and likewise the thief's s2 and
s5, then:

> owner reads `top_` (o4) **before** the thief's CAS (s5) is visible → owner sees
> `t == b`, believes the element is uncontended, takes it.
> thief reads `bottom_` (s2) **before** the owner's decrement (o3) is visible →
> thief sees `b = top_ + 1 > t`, believes the deque non-empty, takes it.

Both take the same element. One task runs twice; another is lost.

**Fix:** o3, o4, s2, s5 are all `seq_cst`, so they all sit in one global total
order **S**. Take any execution and look at S:

- If o3 precedes s2 in S, then s2 (an acquire load) observes the decremented
  `bottom_`; the thief computes `t >= b` and reports `kEmpty`. Owner wins.
- Otherwise s2 precedes o3 in S. Then o3 ⟶ o4 in S (sequenced), and s2 ⟶ s5 in
  S. Now o4 vs s5:
  - o4 before s5 in S: o4 reads `top_` *after* nothing incremented it, so the
    owner proceeds to its own CAS o6 (also `seq_cst`, also in S). o6 and s5
    target the same `top_` value; the atomic CAS lets exactly one succeed.
  - s5 before o4 in S: o4 observes the thief's incremented `top_` → owner sees
    `t > b` → owner takes the empty branch, returns nothing. Thief wins.

Every case: **at most one taker.** This is the classic Dekker / StoreLoad idiom;
the seq_cst operations are the software equivalent of an `mfence` / `dmb ish`
on each side.

`acq_rel` on the CASes is **not** sufficient: an `acq_rel` CAS is ordered
against release/acquire operations on the same location, but it does not join
the single total order S, so on ARM the o4/s5 step above can still go wrong.

---

## 4. The CAS on `top_`, and ABA

`top_` is a 64-bit signed counter that is **only ever incremented** and never
wrapped. The CAS in `steal()` (s5) and `pop()` (o6) is
`compare_exchange_strong(expected=t, desired=t+1)`.

ABA needs a value to change A→B→A between a thread's read and its CAS. Here the
"value" is `top_`, and it never decreases, so it can never return to a previous
value. To wrap a 64-bit counter you would need 2⁶⁴ successful steals; at one per
nanosecond that is ~585 years. So the CAS is ABA-free **by construction**, with
no tag bits, no double-width CAS, no hazard pointers needed for `top_` itself.

(Buffer *memory* is a different question — see §7.)

Failure ordering `relaxed`: on a failed CAS we took nothing and will either
retry via another victim (`steal`) or restore state and return empty (`pop`).
We only need the refreshed `expected`, which the CAS writes regardless of
ordering. Paying `acquire` on the failure path would be a barrier on the
contended retry path for no benefit.

---

## 5. Standalone fences vs per-operation ordering

LPCN Figure 1 publishes the element with `atomic_thread_fence(release)` between
the slot store and a *relaxed* `bottom_` store, and separates the owner/thief
last-element accesses with `atomic_thread_fence(seq_cst)`. That is textbook and
correct on hardware.

TaskForge does **not** use standalone fences. It carries the same orderings on
the operations: a `release` store on `bottom_` in `push` (p5), and `seq_cst` on
the four `bottom_`/`top_` accesses that form S (o3, o4, s2, s5).

Why the change: **ThreadSanitizer does not reliably model
`std::atomic_thread_fence`** (a documented limitation). With the fence version,
under the pool's workload — every thread rapidly alternating owner ops on its
own deque and thief ops on others — TSan on arm64 deadlocked inside its own
runtime. The per-operation version is:

- **Provably equivalent.** The release store on `bottom_` gives the same
  release→acquire edge to `steal()` that fence + relaxed store gave. The four
  seq_cst operations give the same S that two seq_cst fences gave; §3's argument
  is unchanged.
- **What real implementations do.** crossbeam-deque (Rust) and Taskflow-style
  C++ deques carry the orderings on the ops for exactly this reason.
- **TSan-clean**, on Linux x86-64 CI and on the arm64 dev box.

Cost on ARM is essentially unchanged: `bottom_.store(seq_cst)` compiles to
roughly what a preceding seq_cst fence would; the `push` side arguably gets
*cheaper* (a `stlr` release store vs a `dmb ish` full fence).

This is the single most important "what didn't work" in the project — see the
README section of that name.

---

## 6. x86-TSO vs ARM: what each ordering costs

| ordering | x86-64 (TSO) | ARM (AArch64) |
|---|---|---|
| `relaxed` load/store | plain `mov` | plain `ldr`/`str` |
| `acquire` load | plain `mov` (loads already acquire on TSO) | `ldar` (or `ldapr`) |
| `release` store | plain `mov` (stores already release on TSO) | `stlr` |
| `seq_cst` load | plain `mov` | `ldar` |
| `seq_cst` store | `xchg` **or** `mov` + `mfence` | `stlr` + trailing `dmb ish` (impl-dependent) |
| `seq_cst` fence | `mfence` | `dmb ish` |
| `seq_cst` RMW/CAS | `lock cmpxchg` (already a full barrier) | `ldaxr`/`stlxr` loop + barrier |

Consequences for this deque:

- On **x86**, almost every non-`relaxed` order in `push`/`steal` is *free* — the
  hardware already gives acquire loads and release stores. The only real cost is
  the StoreLoad barrier implied by the seq_cst `bottom_` store in `pop` and the
  seq_cst load in `steal`. This is why the Stage 9 bug (`relaxed` publish in
  `push`) is invisible on x86: x86 never reorders the slot store past the
  `bottom_` store anyway.
- On **ARM**, the acquire/release/seq_cst annotations each emit a barrier or a
  barriered instruction. This is where a relaxed-ordering mistake actually
  reorders and gets caught — Stage 9 fails every stress run on the Apple M5.

The project's stance: **test both.** CI runs the suite under TSan on
`ubuntu-24.04` (x86-64) *and* `macos-14` (arm64). A relaxed-ordering bug that
x86 papers over surfaces on the ARM job or the ARM dev box.

---

## 7. Memory reclamation of grown buffers

When `push` fills the ring, `grow()` allocates a 2× buffer, copies the live
range, and publishes it with `ring_.store(release)`. The **old buffer is not
freed** — a thief may be inside `steal()` holding `old_ring` and about to read a
slot from it.

TaskForge keeps every ring ever allocated on an owner-private
`std::vector<unique_ptr<Ring>> owned_rings_`, freed only in the deque
destructor, which runs after all pool threads have joined.

- The vector is touched **only by the owner** and **only on the cold `grow`
  path**, so its (possibly allocating) `push_back` never hits the steady-state
  hot path.
- Cost: `O(log(max size))` retired buffers held for the deque's lifetime. For a
  scheduler, deques live as long as the pool, and a worker deque doubling from
  1024 to, say, 1M retires ~10 buffers totalling ~2× the final size. Negligible.
- A general-purpose deque that is created and destroyed frequently would need
  **hazard pointers** or **epoch-based reclamation** to free old buffers
  promptly. That is the right trade for a library; it is overkill here and would
  add a per-`steal` hazard-pointer publish to the hot path.

---

## 8. The thread pool

`include/taskforge/thread_pool.hpp`. One `ChaseLevDeque<Task*>` per worker.

### Why external submits go through an injection queue

Chase–Lev requires a single owner per deque. An external producer calling
`push()` on a worker's deque would be a second writer → UB. So external
`submit()` puts the task on a **mutex-guarded injection queue** (`std::deque` +
`std::mutex`), and workers check that queue after their own deque and after a
steal sweep.

`submit()` called **from inside a running task** is the fast path: a
`thread_local` pointer identifies the current worker, and the task is pushed
straight onto that worker's own deque — no lock, and fork–join recursion stays
on the deque where stealing can rebalance it.

### `pending_` accounting

`std::atomic<int64_t> pending_` counts submitted-but-not-finished tasks.
`submit` does `fetch_add(1, acq_rel)` *before* the task is visible to any
worker; `run_task` does `fetch_sub(1, acq_rel)` *after* `fn()` returns. Children
are submitted (incrementing `pending_`) before their parent's `fn()` returns and
thus before the parent's decrement, so `pending_` never hits zero while work
remains. `wait_idle()` blocks on a condvar until `pending_ == 0`; the
`fetch_sub` that reaches zero takes `idle_mtx_` and notifies. `acq_rel` on the
decrement means the zero-observer happens-after every child increment.

### Backoff state machine

An idle worker escalates:

```
  fails ≤ spin_iters              cpu_relax()        (PAUSE / YIELD hint; ~ns)
  spin_iters < fails ≤ +yield     std::this_thread::yield()
  fails > spin_iters + yield      sleep on sleep_cv_ with a 500 µs timeout
  (after any wake)                fails = spin_iters   -> re-check quickly
```

Defaults `spin_iters=128`, `yield_iters=64`, `sleep_backstop=500µs`; Stage 5/7
benchmarks exist to tune them per machine.

**Lost-wakeup handling.** `submit` sets an atomic `work_hint_` flag and calls
`sleep_cv_.notify_one()`. A worker that is between "found no work" and "entered
`wait_for`" when a `submit` lands would miss the notify — but its `wait_for`
predicate also tests `work_hint_`, so it won't sleep; and if another worker
already consumed `work_hint_`, the 500 µs timeout bounds the stall. A production
pool would use an eventcount (a futex-backed counter) to make every wakeup
precise; the timeout backstop is the simpler 90%-solution and is called out here
as a known simplification.

### Victim selection

`acquire_task` steals from `2·(n-1)` **random** victims before falling back to
the injection queue. Round-robin was rejected: under load skew it marches every
idle thief across the same victim sequence in lockstep, so they convoy on
whichever deque is hot and repeatedly lose the CAS to each other. Random
spreads the attempts; the expected number of tries to hit a specific
non-empty victim is small, and different thieves diverge immediately.

`std::minstd_rand` per worker, seeded from the worker id. Its low bits are
famously weak, but `rng() % n` over thousands of calls still covers every
victim; the benchmark's balanced `tasks_executed` across workers confirms it in
practice.

### `help_until(pred)` — fork–join without deadlock

A task that spawns children and needs their results calls
`pool.help_until([&]{ return children_done; })` instead of blocking. Inside,
the worker keeps running tasks (its own, stolen, or injected) until `pred()` is
true. It never blocks a thread, so N parents waiting on children can't starve
the pool. This is **child stealing with explicit helping**, not continuation
stealing (Java `ForkJoinPool`) — simpler, no continuation capture, at the cost
that the waiting frame stays on the stack (depth bounded by task nesting, e.g.
`log n` for divide-and-conquer).

---

## 9. The DAG resolver

`include/taskforge/dag.hpp`. Build the graph single-threaded (`add_node`,
`add_edge`), then `run(pool)`:

1. **Cycle check** — Kahn topological sort over the declared edges, O(V+E). The
   whole graph is known before execution, so a cycle is caught here and `run`
   throws `CycleError` before any task is submitted. `DagCycle.NoTaskRunsWhenCycleRejected`
   pins this.
2. Reset each node's `remaining` (unresolved-prerequisite count) to its
   in-degree.
3. Submit every zero-in-degree node.
4. Each node's task, after `fn()` returns, does for each successor `s`:
   `if (s.remaining.fetch_sub(1, acq_rel) == 1) submit(s);`

The `fetch_sub(acq_rel)` is the interesting part:

- **release**: publishes everything this node's `fn()` wrote, so a successor
  that runs later sees it.
- **acquire**: the decrement that reads `1` (i.e. brings `remaining` to `0`) is
  the one that schedules the successor. `acquire` means that decrement has
  observed the release-decrements of **all** other predecessors — so by the time
  the successor runs, every predecessor's writes are visible to it. `submit()`'s
  own release (deque push / injection) → acquire (pop / steal) then carries that
  happens-before across to whichever worker actually runs the successor.

`relaxed` here would let a successor start before a predecessor's results were
visible. `seq_cst` would be a global barrier per edge for no added guarantee.

**Exception policy for a node:** the node's exception is caught, the first one
retained, and successors are **still released** so `run` cannot hang; `run`
re-raises the first exception after `wait_idle()`. A node that throws therefore
does not stall the DAG, but its successors may see incomplete inputs — don't
throw from a node unless you mean to abort.

---

## 10. Stage 9: the deliberately wrong ordering

`include/taskforge/chase_lev_deque_relaxed.hpp` is the correct deque with **p5
changed from `release` to `relaxed`**:

```diff
   r->put(b, x);
-  bottom_.store(b + 1, std::memory_order_release);
+  bottom_.store(b + 1, std::memory_order_relaxed);   // BUG
```

That store is the *only* happens-before edge between p4 (the slot write) and s4
(the thief's slot read). Remove it and:

- there is no synchronizes-with between the writer and the reader of the slot;
- with `std::atomic` slots this would be a benign-looking relaxed/relaxed pair
  that TSan stays quiet about, so the buggy variant also makes the slots **plain
  `T`** (a two-word cell), turning the latent race into real UB.

Result, as measured:

| exercised how | outcome |
|---|---|
| single-threaded / light | pass — casual testing misses it |
| `-O2` stress, x86-64 (TSO) | pass — x86 never reorders p4 past p5 (§6) |
| `-O2` stress, Apple M5 | **fail every run** — ~40k duplicate (stale) values per ~12M ops |
| TSan, Linux x86-64 (CI) | **data race reported** on the cell |
| TSan, Apple M5 | detects it, but the arm64 sanitizer runtime deadlocks its own report path under concurrent races (README "what didn't work") |

`ctest -L buggy-demo` / `make buggy-demo` run it. The CI job
`buggy-ordering-demo` **fails if the variant ever comes back clean** — the demo
is itself tested.

The point: for lock-free code, the memory model is the specification. A green
stress run on one machine is not evidence of correctness.

---

## 11. Exception safety

- **Detached tasks** (`submit`): `run_task` wraps `fn()` in `try/catch(...)`.
  The exception count is bumped (`exceptions_`, relaxed) and the first
  `exception_ptr` is retained under `exc_mtx_`. The worker then continues
  normally — the pool never calls `std::terminate`, never deadlocks, and
  `pending_` is still decremented for the failed task.
- **Futures** (`submit_future`): the callable runs inside a
  `std::packaged_task`, which stores a thrown exception in the shared state;
  `future::get()` re-throws it to the caller. The pool-level counters are not
  touched in this path (the caller owns the outcome).
- **DAG nodes**: see §9.
- Tested in `test_exception_safety.cpp` / `verify_exception.cpp`: non-`std::`
  exceptions (`throw 42`), scattered throws in a recursive workload, "pool still
  usable afterwards", and `first_exception()` round-trips the message.

---

## 12. Clean shutdown

Three states the pool must survive being destroyed in — all tested
(`test_pool_shutdown.cpp`):

| state | what happens |
|---|---|
| **tasks in flight** | `~ThreadPool` → `shutdown()` sets `done_` and joins. Workers finish the task they're running, then exit once `done_ && globally_idle()`. |
| **tasks queued** | Graceful: workers keep draining their deques + the injection queue until everything has *run*, then exit. `~ThreadPool` returns only after that. `JoinWithDeepBacklogRunsItAll` submits 40 000 tasks then destructs with no `wait_idle()` and asserts all 40 000 ran. |
| **workers mid-steal** | A worker mid-`steal()` either completes it (runs the task) or sees empty and loops to the `done_ && globally_idle()` check. The seq_cst operations make the steal itself safe regardless of shutdown timing. |

`shutdown()` sets `done_`, takes and releases `sleep_mtx_` once (a barrier
against a worker that is between its predicate check and `wait_for`), then
`notify_all()` and joins.

`globally_idle()` = every deque empty **and** the injection queue empty. It is
only consulted together with `done_`, so no new external work can arrive; and a
child pushed by a still-running task lands on **that task's own worker's**
deque, which that worker drains before it checks `globally_idle()`. So once every
worker has exited, nothing is left un-run.

`shutdown_now()` sets a separate `drop_` flag → workers exit as soon as they
notice, and the destructor `delete`s the undrained `Task*`s (decrementing
`pending_` for each) so ASan sees no leak.

**Contract:** because a task runs right up until the workers are joined, any
state a task captures by reference must outlive the pool. Declare the pool last
among locals, or `wait_idle()` before that state goes out of scope. (An earlier
version of `test_pool_shutdown.cpp` got this wrong and ASan caught the
use-after-free — the fix is a one-liner and the rule is now in the pool's header
comment.)
