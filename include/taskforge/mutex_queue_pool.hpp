// mutex_queue_pool.hpp
// =============================================================================
// Baseline #1: the obvious thread pool. One global std::mutex guarding one
// std::queue, one condition_variable. No stealing, no per-worker state, one
// point of contention that every worker and every producer serializes on.
//
// This is the thing the work-stealing pool has to beat. It exposes the same API
// (see thread_pool.hpp) so bench/ can template over the pool type. It is
// deliberately straightforward -- the interesting numbers come from comparing
// it to ThreadPool across the task-granularity sweep, where this design is fine
// for coarse tasks and falls apart for fine ones.
// =============================================================================

#pragma once

#include "taskforge/cpu_relax.hpp"
#include "taskforge/task.hpp"
#include "taskforge/thread_pool.hpp"  // for WorkerCounters / PoolStats

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace taskforge {

class MutexQueuePool {
 public:
  explicit MutexQueuePool(unsigned threads = 0)
      : n_(threads ? threads : ThreadPool::default_thread_count()) {
    workers_.reserve(n_);
    for (unsigned i = 0; i < n_; ++i)
      workers_.emplace_back([this] { worker_main(); });
  }

  ~MutexQueuePool() { shutdown(); }

  MutexQueuePool(const MutexQueuePool&) = delete;
  MutexQueuePool& operator=(const MutexQueuePool&) = delete;

  template <class F>
  void submit(F&& fn) {
    enqueue(new Task(std::function<void()>(std::forward<F>(fn))));
  }

  template <class F>
  auto submit_future(F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
    using R = std::invoke_result_t<std::decay_t<F>>;
    auto pt = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
    std::future<R> fut = pt->get_future();
    enqueue(new Task([pt] { (*pt)(); }));
    return fut;
  }

  void wait_idle() {
    std::unique_lock<std::mutex> lk(done_mtx_);
    done_cv_.wait(lk, [this] { return pending_.load(std::memory_order_acquire) == 0; });
  }

  // Fork-join helper: run queued tasks until pred(), never blocking a thread.
  template <class Pred>
  void help_until(Pred done) {
    while (!done()) {
      Task* t = try_pop();
      if (t) {
        run_task(t);
      } else if (done()) {
        return;
      } else {
        cpu_relax();
      }
    }
  }

  void shutdown() {
    if (done_.exchange(true, std::memory_order_acq_rel)) return;
    q_cv_.notify_all();
    for (auto& t : workers_)
      if (t.joinable()) t.join();
  }

  void shutdown_now() {
    drop_.store(true, std::memory_order_release);
    shutdown();
    std::lock_guard<std::mutex> g(q_mtx_);
    while (!q_.empty()) {
      delete q_.front();
      q_.pop();
      pending_.fetch_sub(1, std::memory_order_acq_rel);
    }
  }

  unsigned thread_count() const { return n_; }
  std::uint64_t exception_count() const {
    return exceptions_.load(std::memory_order_relaxed);
  }
  std::exception_ptr first_exception() const {
    std::lock_guard<std::mutex> g(exc_mtx_);
    return first_exc_;
  }
  std::int64_t pending() const { return pending_.load(std::memory_order_relaxed); }

  // No per-worker instrumentation in the baseline; return a zeroed block so
  // callers that template over pool type still compile.
  PoolStats stats() const {
    PoolStats s;
    s.per_worker.resize(n_);
    return s;
  }

 private:
  void enqueue(Task* raw) {
    std::unique_ptr<Task> t(raw);
    if (done_.load(std::memory_order_acquire))
      throw std::runtime_error("MutexQueuePool::submit after shutdown");
    pending_.fetch_add(1, std::memory_order_acq_rel);
    {
      std::lock_guard<std::mutex> g(q_mtx_);
      q_.push(t.release());
    }
    q_cv_.notify_one();
  }

  Task* try_pop() {
    std::lock_guard<std::mutex> g(q_mtx_);
    if (q_.empty()) return nullptr;
    Task* t = q_.front();
    q_.pop();
    return t;
  }

  void run_task(Task* raw) {
    std::unique_ptr<Task> t(raw);
    try {
      t->run();
    } catch (...) {
      exceptions_.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard<std::mutex> g(exc_mtx_);
      if (!first_exc_) first_exc_ = std::current_exception();
    }
    if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard<std::mutex> g(done_mtx_);
      done_cv_.notify_all();
    }
  }

  void worker_main() {
    for (;;) {
      Task* t = nullptr;
      {
        std::unique_lock<std::mutex> lk(q_mtx_);
        q_cv_.wait(lk, [this] {
          return !q_.empty() || done_.load(std::memory_order_acquire) ||
                 drop_.load(std::memory_order_acquire);
        });
        if (drop_.load(std::memory_order_acquire)) return;
        if (q_.empty()) {
          // Woken for shutdown with nothing left: graceful drain complete.
          if (done_.load(std::memory_order_acquire)) return;
          continue;
        }
        t = q_.front();
        q_.pop();
      }
      run_task(t);
    }
  }

  const unsigned n_;
  std::vector<std::thread> workers_;

  mutable std::mutex q_mtx_;
  std::condition_variable q_cv_;
  std::queue<Task*> q_;

  std::atomic<bool> done_{false};
  std::atomic<bool> drop_{false};
  std::atomic<std::int64_t> pending_{0};

  std::mutex done_mtx_;
  std::condition_variable done_cv_;

  std::atomic<std::uint64_t> exceptions_{0};
  mutable std::mutex exc_mtx_;
  std::exception_ptr first_exc_;
};

}  // namespace taskforge
