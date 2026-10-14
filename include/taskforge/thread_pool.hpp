// thread_pool.hpp
// =============================================================================
// A work-stealing thread pool built on ChaseLevDeque.
//
//   * One deque per worker. A worker runs its own tasks LIFO (cache-hot), and
//     when it runs dry it steals FIFO from a random victim.
//   * External submit() goes through a mutex-guarded injection queue -- never
//     straight onto a worker's deque, because Chase-Lev allows exactly one
//     owner per deque. submit() called *from inside a running task* is the
//     fast path: it pushes onto the current worker's own deque.
//   * Idle workers escalate: bounded PAUSE spin -> yield -> sleep on a condvar
//     with a short timeout backstop against lost wakeups.
//   * Shutdown is graceful by default: every queued task still runs before the
//     workers exit. shutdown_now() drops what is still queued.
//   * A task that throws is caught, counted, and the first exception retained;
//     the pool never terminates and never deadlocks. Futures get the exception.
//
// Lifetime: tasks run until ~ThreadPool has joined the workers, so any state a
// task captures by reference must outlive the pool. Declare the pool last among
// locals, or call wait_idle()/shutdown() before that state goes out of scope.
//
// Common pool API (MutexQueuePool and AsyncPool match it, so bench/ is generic):
//   explicit Pool(unsigned threads);
//   ~Pool();                                   // graceful shutdown + join
//   void   submit(F&&);                        // fire and forget
//   auto   submit_future(F&&) -> std::future<result_of_F>;
//   void   wait_idle();                        // block until all work is done
//   template<class Pred> void help_until(Pred);// run tasks until Pred (fork-join)
//   unsigned      thread_count() const;
//   std::uint64_t exception_count() const;
//   std::exception_ptr first_exception() const;
// =============================================================================

#pragma once

#include "taskforge/chase_lev_deque.hpp"
#include "taskforge/cpu_relax.hpp"
#include "taskforge/task.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace taskforge {

// Per-worker instrumentation, PLAIN snapshot: what stats() hands back. Every
// field is a point-in-time copy (see WorkerCountersLive below for the live
// counters it is copied from), so this type is an ordinary copyable POD and
// costs nothing extra to store in PoolStats::per_worker.
struct WorkerCounters {
  std::uint64_t tasks_executed = 0;
  std::uint64_t own_pops = 0;
  std::uint64_t steals_success = 0;
  std::uint64_t steals_abort = 0;   // lost the CAS race
  std::uint64_t steals_empty = 0;   // victim had nothing
  std::uint64_t inject_takes = 0;
  std::uint64_t spin_iters = 0;     // PAUSE-spin iterations (proxy for spin time)
  std::uint64_t yield_iters = 0;    // yield() iterations
  std::uint64_t sleeps = 0;         // times this worker blocked on the condvar
  std::uint64_t sleep_ns = 0;       // wall time spent blocked on the condvar
};

// The LIVE counters, one block per worker, mutated only by the owning worker
// thread. A worker keeps touching these for as long as it is alive -- not just
// while running tasks: every failed steal attempt, spin iteration, and sleep in
// the idle backoff loop (worker_main) increments one of them too. So
// pool.wait_idle() returning (pending_ == 0) does NOT mean these have gone
// quiet; a worker with nothing left to do is still spinning through
// acquire_task() and bumping steals_empty / spin_iters on every pass. stats()
// can therefore be called at any time while the pool is alive -- that is
// exactly why these are atomics (relaxed: the fields are independent counters,
// not a consistent multi-field snapshot, so there is nothing stronger to buy).
// A plain (non-atomic) field here, read concurrently by stats() while a worker
// is mid-backoff, is a real data race (caught by TSan on Linux x86-64 in the
// very first CI run -- the earlier version of this comment, which claimed
// reading after wait_idle() was safe, was simply wrong).
//
// alignas keeps each worker's block on its own cache line -- Stage 8
// deliberately removes that alignment to measure false sharing.
struct alignas(TF_CACHELINE) WorkerCountersLive {
  std::atomic<std::uint64_t> tasks_executed{0};
  std::atomic<std::uint64_t> own_pops{0};
  std::atomic<std::uint64_t> steals_success{0};
  std::atomic<std::uint64_t> steals_abort{0};
  std::atomic<std::uint64_t> steals_empty{0};
  std::atomic<std::uint64_t> inject_takes{0};
  std::atomic<std::uint64_t> spin_iters{0};
  std::atomic<std::uint64_t> yield_iters{0};
  std::atomic<std::uint64_t> sleeps{0};
  std::atomic<std::uint64_t> sleep_ns{0};

  WorkerCounters snapshot() const {
    WorkerCounters s;
    s.tasks_executed = tasks_executed.load(std::memory_order_relaxed);
    s.own_pops = own_pops.load(std::memory_order_relaxed);
    s.steals_success = steals_success.load(std::memory_order_relaxed);
    s.steals_abort = steals_abort.load(std::memory_order_relaxed);
    s.steals_empty = steals_empty.load(std::memory_order_relaxed);
    s.inject_takes = inject_takes.load(std::memory_order_relaxed);
    s.spin_iters = spin_iters.load(std::memory_order_relaxed);
    s.yield_iters = yield_iters.load(std::memory_order_relaxed);
    s.sleeps = sleeps.load(std::memory_order_relaxed);
    s.sleep_ns = sleep_ns.load(std::memory_order_relaxed);
    return s;
  }
};

struct PoolStats {
  std::vector<WorkerCounters> per_worker;
  std::uint64_t total(std::uint64_t WorkerCounters::*field) const {
    std::uint64_t s = 0;
    for (const auto& w : per_worker) s += w.*field;
    return s;
  }
};

// Backoff schedule for an idle worker: spin this many PAUSE iterations, then
// this many yields, then sleep on the condvar. Kept small; Stage 5/7 measure
// whether these are the right numbers. Hoisted out of ThreadPool so it can be a
// defaulted constructor argument (a nested type with default member
// initializers cannot be `= {}`-defaulted from within the enclosing class).
struct ThreadPoolOptions {
  std::uint32_t spin_iters = 128;
  std::uint32_t yield_iters = 64;
  std::chrono::microseconds sleep_backstop{500};  // lost-wakeup safety net
};

class ThreadPool {
 public:
  using Options = ThreadPoolOptions;

  explicit ThreadPool(unsigned threads = 0, Options opt = {})
      : opt_(opt), n_(threads ? threads : default_thread_count()), workers_(n_) {
    for (unsigned i = 0; i < n_; ++i) workers_[i] = std::make_unique<Worker>(i);
    for (unsigned i = 0; i < n_; ++i)
      workers_[i]->thread = std::thread([this, i] { worker_main(workers_[i].get()); });
  }

  ~ThreadPool() { shutdown(); }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // ----------------------------------------------------------------- submit
  template <class F>
  void submit(F&& fn) {
    enqueue(new Task(std::function<void()>(std::forward<F>(fn))));
  }

  template <class F>
  auto submit_future(F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
    using R = std::invoke_result_t<std::decay_t<F>>;
    auto pt = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
    std::future<R> fut = pt->get_future();
    // packaged_task routes a thrown exception into the future automatically.
    enqueue(new Task([pt] { (*pt)(); }));
    return fut;
  }

  // --------------------------------------------------------------- wait_idle
  // Block (from an EXTERNAL thread) until every submitted task has finished.
  // Do not call from inside a task -- use help_until() for fork-join.
  void wait_idle() {
    std::unique_lock<std::mutex> lk(idle_mtx_);
    idle_cv_.wait(lk, [this] { return pending_.load(std::memory_order_acquire) == 0; });
  }

  // -------------------------------------------------------------- help_until
  // Run tasks (own / stolen / injected) until `done()` is true. This is how a
  // task waits for the children it spawned without deadlocking the pool: the
  // waiting worker keeps doing useful work instead of blocking a thread. Safe
  // to nest (merge-sort-style recursion); depth is bounded by task nesting,
  // which the caller controls.
  template <class Pred>
  void help_until(Pred done) {
    Worker* w = (t_pool_ == this) ? t_worker_ : nullptr;
    while (!done()) {
      Task* t = w ? acquire_task(w) : acquire_task_external();
      if (t) {
        run_task(t, w);
      } else if (done()) {
        return;
      } else {
        cpu_relax();
      }
    }
  }

  // --------------------------------------------------------------- shutdown
  // Graceful: signal done, let workers drain every remaining task, then join.
  void shutdown() {
    if (done_.exchange(true, std::memory_order_acq_rel)) return;
    { std::lock_guard<std::mutex> g(sleep_mtx_); }  // serialize with sleepers
    sleep_cv_.notify_all();
    for (auto& w : workers_)
      if (w->thread.joinable()) w->thread.join();
    // Workers exit only after globally_idle(); nothing is left to run.
  }

  // Abandon queued work: workers exit as soon as they notice, and any tasks
  // still sitting in deques / injection are deleted unrun.
  void shutdown_now() {
    drop_.store(true, std::memory_order_release);
    shutdown();
    drain_and_discard();
  }

  // --------------------------------------------------------------- observers
  unsigned thread_count() const { return n_; }
  std::uint64_t exception_count() const {
    return exceptions_.load(std::memory_order_relaxed);
  }
  std::exception_ptr first_exception() const {
    std::lock_guard<std::mutex> g(exc_mtx_);
    return first_exc_;
  }
  std::int64_t pending() const { return pending_.load(std::memory_order_relaxed); }

  // Safe to call at any time, including while the pool is still running --
  // each field is an independent relaxed-atomic snapshot (see
  // WorkerCountersLive). The whole struct is not a consistent instant in time
  // across fields; for that, call after shutdown()/destruction.
  PoolStats stats() const {
    PoolStats s;
    s.per_worker.reserve(n_);
    for (auto& w : workers_) s.per_worker.push_back(w->counters.snapshot());
    return s;
  }

  static unsigned default_thread_count() {
    unsigned h = std::thread::hardware_concurrency();
    return h ? h : 4;
  }

 private:
  struct alignas(TF_CACHELINE) Worker {
    explicit Worker(unsigned id_)
        : id(id_), deque(1024), rng(0x9E3779B9u ^ (id_ * 2654435761u)) {}
    unsigned id;
    ChaseLevDeque<Task*> deque;
    std::thread thread;
    std::minstd_rand rng;
    WorkerCountersLive counters;
  };

  // Thread-locals: which pool/worker is the calling thread, if it is one of
  // ours. Lets submit()/help_until() take the fast in-task path.
  static thread_local ThreadPool* t_pool_;
  static thread_local Worker* t_worker_;

  // ------------------------------------------------------------- enqueue
  void enqueue(Task* raw) {
    std::unique_ptr<Task> t(raw);
    if (done_.load(std::memory_order_acquire)) {
      // Reject post-shutdown submits loudly rather than leaking the task or
      // running it on a pool that is tearing down.
      throw std::runtime_error("ThreadPool::submit after shutdown");
    }
    pending_.fetch_add(1, std::memory_order_acq_rel);

    if (t_pool_ == this && t_worker_ != nullptr) {
      // Fast path: recursive submit from a worker -> its own deque, no lock.
      t_worker_->deque.push(t.release());
    } else {
      std::lock_guard<std::mutex> g(inj_mtx_);
      inj_.push_back(t.release());
    }
    // Wake one sleeper. work_hint_ + the sleep backstop cover the race where
    // this store/notify lands between a worker's "no work" check and its wait.
    work_hint_.store(true, std::memory_order_release);
    sleep_cv_.notify_one();
  }

  // ------------------------------------------------------ task acquisition
  // Own deque, then a bounded random-victim steal sweep, then injection queue.
  Task* acquire_task(Worker* w) {
    if (auto v = w->deque.pop()) {
      w->counters.own_pops.fetch_add(1, std::memory_order_relaxed);
      return *v;
    }
    // Random victim selection. Round-robin would march every idle thief across
    // the same victims in lockstep and convoy on a hot one under load skew;
    // random spreads the steal attempts. See DESIGN.md "victim selection".
    if (n_ > 1) {
      const unsigned attempts = 2 * (n_ - 1);
      for (unsigned k = 0; k < attempts; ++k) {
        const unsigned v = static_cast<unsigned>(w->rng() % n_);
        if (v == w->id) continue;
        Task* out = nullptr;
        switch (workers_[v]->deque.steal(out)) {
          case StealStatus::kSuccess:
            w->counters.steals_success.fetch_add(1, std::memory_order_relaxed);
            return out;
          case StealStatus::kAbort:
            w->counters.steals_abort.fetch_add(1, std::memory_order_relaxed);
            break;  // try another victim rather than retry in place
          case StealStatus::kEmpty:
            w->counters.steals_empty.fetch_add(1, std::memory_order_relaxed);
            break;
        }
      }
    }
    if (Task* t = take_injected()) {
      w->counters.inject_takes.fetch_add(1, std::memory_order_relaxed);
      return t;
    }
    return nullptr;
  }

  // For an external thread inside help_until(): steal from anyone, then inject.
  Task* acquire_task_external() {
    for (unsigned v = 0; v < n_; ++v) {
      Task* out = nullptr;
      if (workers_[v]->deque.steal(out) == StealStatus::kSuccess) return out;
    }
    return take_injected();
  }

  Task* take_injected() {
    std::lock_guard<std::mutex> g(inj_mtx_);
    if (inj_.empty()) return nullptr;
    Task* t = inj_.front();
    inj_.pop_front();
    return t;
  }

  // --------------------------------------------------------------- run_task
  void run_task(Task* raw, Worker* w) {
    std::unique_ptr<Task> t(raw);
    try {
      t->run();
    } catch (...) {
      exceptions_.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard<std::mutex> g(exc_mtx_);
      if (!first_exc_) first_exc_ = std::current_exception();
    }
    if (w) w->counters.tasks_executed.fetch_add(1, std::memory_order_relaxed);

    // Completion accounting. acq_rel so the decrement that reaches zero
    // happens-after every child increment this task made (children bump
    // pending_ before the parent's run() returns).
    if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard<std::mutex> g(idle_mtx_);
      idle_cv_.notify_all();
    }
  }

  // --------------------------------------------------------- worker main
  void worker_main(Worker* w) {
    t_pool_ = this;
    t_worker_ = w;

    std::uint64_t fails = 0;
    while (true) {
      if (drop_.load(std::memory_order_acquire)) break;

      if (Task* t = acquire_task(w)) {
        run_task(t, w);
        fails = 0;
        continue;
      }

      // No work found. If shutting down and the whole pool is drained, exit.
      if (done_.load(std::memory_order_acquire) && globally_idle()) break;

      ++fails;
      if (fails <= opt_.spin_iters) {
        w->counters.spin_iters.fetch_add(1, std::memory_order_relaxed);
        cpu_relax();
        continue;
      }
      if (fails <= opt_.spin_iters + opt_.yield_iters) {
        w->counters.yield_iters.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
        continue;
      }

      // Sleep until woken by a submit / shutdown, or the backstop fires.
      w->counters.sleeps.fetch_add(1, std::memory_order_relaxed);
      {
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lk(sleep_mtx_);
        sleep_cv_.wait_for(lk, opt_.sleep_backstop, [this] {
          return done_.load(std::memory_order_acquire) ||
                 drop_.load(std::memory_order_acquire) ||
                 work_hint_.load(std::memory_order_acquire);
        });
        work_hint_.store(false, std::memory_order_relaxed);
        const auto slept_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        w->counters.sleep_ns.fetch_add(slept_ns, std::memory_order_relaxed);
      }
      // After waking, drop back to the spin tier so a burst of submits does not
      // pay a full sleep each time.
      fails = opt_.spin_iters;
    }
  }

  // True if every deque is empty and the injection queue is empty. Only
  // consulted alongside done_ (no more submits can arrive), so a worker that
  // sees this can safely exit; any child a still-running task pushes lands on
  // that task's own worker, which drains its own deque before checking here.
  bool globally_idle() const {
    for (auto& w : workers_)
      if (!w->deque.empty()) return false;
    std::lock_guard<std::mutex> g(inj_mtx_);
    return inj_.empty();
  }

  void drain_and_discard() {
    for (auto& w : workers_) {
      while (auto v = w->deque.pop()) {
        delete *v;
        pending_.fetch_sub(1, std::memory_order_acq_rel);
      }
    }
    std::lock_guard<std::mutex> g(inj_mtx_);
    while (!inj_.empty()) {
      delete inj_.front();
      inj_.pop_front();
      pending_.fetch_sub(1, std::memory_order_acq_rel);
    }
  }

  // ------------------------------------------------------------------ data
  const Options opt_;
  const unsigned n_;
  std::vector<std::unique_ptr<Worker>> workers_;

  mutable std::mutex inj_mtx_;
  std::deque<Task*> inj_;

  std::atomic<bool> done_{false};   // graceful stop requested
  std::atomic<bool> drop_{false};   // hard stop (shutdown_now)

  // Outstanding tasks: submitted and not yet finished. wait_idle() waits for 0.
  std::atomic<std::int64_t> pending_{0};

  std::mutex sleep_mtx_;
  std::condition_variable sleep_cv_;
  std::atomic<bool> work_hint_{false};

  std::mutex idle_mtx_;
  std::condition_variable idle_cv_;

  std::atomic<std::uint64_t> exceptions_{0};
  mutable std::mutex exc_mtx_;
  std::exception_ptr first_exc_;
};

// Out-of-line thread_local definitions (header-only: inline).
inline thread_local ThreadPool* ThreadPool::t_pool_ = nullptr;
inline thread_local ThreadPool::Worker* ThreadPool::t_worker_ = nullptr;

}  // namespace taskforge
