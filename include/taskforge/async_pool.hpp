// async_pool.hpp
// =============================================================================
// Baseline #2: "just use std::async". Every submit() is
// std::async(std::launch::async, ...). No queue, no worker threads of our own --
// the implementation spins up a thread per task (libstdc++ and libc++ both do,
// in practice; the standard permits pooling but neither does it).
//
// Same API surface as ThreadPool so bench/ can template over it. The expected
// result of the granularity sweep: fine with millisecond tasks, catastrophic
// with microsecond tasks, where thread creation/teardown dwarfs the work. That
// contrast is the reason this baseline exists.
// =============================================================================

#pragma once

#include "taskforge/cpu_relax.hpp"
#include "taskforge/thread_pool.hpp"  // for WorkerCounters / PoolStats, default_thread_count

#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <type_traits>
#include <utility>

namespace taskforge {

class AsyncPool {
 public:
  // `threads` is accepted for API parity and reported by thread_count(), but
  // std::async decides its own concurrency.
  explicit AsyncPool(unsigned threads = 0)
      : n_(threads ? threads : ThreadPool::default_thread_count()) {}

  ~AsyncPool() { wait_idle(); }

  AsyncPool(const AsyncPool&) = delete;
  AsyncPool& operator=(const AsyncPool&) = delete;

  template <class F>
  void submit(F&& fn) {
    if (done_.load(std::memory_order_acquire))
      throw std::runtime_error("AsyncPool::submit after shutdown");
    pending_.fetch_add(1, std::memory_order_acq_rel);
    auto fut = std::async(std::launch::async, [this, g = std::forward<F>(fn)]() mutable {
      try {
        g();
      } catch (...) {
        exceptions_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(exc_mtx_);
        if (!first_exc_) first_exc_ = std::current_exception();
      }
      pending_.fetch_sub(1, std::memory_order_acq_rel);
    });
    std::lock_guard<std::mutex> lk(futs_mtx_);
    futs_.push_back(std::move(fut));
  }

  template <class F>
  auto submit_future(F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
    if (done_.load(std::memory_order_acquire))
      throw std::runtime_error("AsyncPool::submit after shutdown");
    pending_.fetch_add(1, std::memory_order_acq_rel);
    return std::async(std::launch::async,
                      [this, g = std::forward<F>(fn)]() mutable
                      -> std::invoke_result_t<std::decay_t<F>> {
      struct Dec {
        AsyncPool* p;
        ~Dec() { p->pending_.fetch_sub(1, std::memory_order_acq_rel); }
      } dec{this};
      return g();  // exception (if any) propagates into the caller's future
    });
  }

  void wait_idle() {
    std::deque<std::future<void>> local;
    {
      std::lock_guard<std::mutex> lk(futs_mtx_);
      local.swap(futs_);
    }
    for (auto& f : local)
      if (f.valid()) f.wait();
    // submit_future() results are not tracked here; pending_ still accounts for
    // them, so spin briefly for those.
    while (pending_.load(std::memory_order_acquire) != 0) cpu_relax();
  }

  template <class Pred>
  void help_until(Pred done) {
    // No queue to drain -- std::async is already running the work elsewhere.
    while (!done()) cpu_relax();
  }

  void shutdown() {
    done_.store(true, std::memory_order_release);
    wait_idle();
  }
  void shutdown_now() { shutdown(); }  // nothing queued to discard

  unsigned thread_count() const { return n_; }
  std::uint64_t exception_count() const {
    return exceptions_.load(std::memory_order_relaxed);
  }
  std::exception_ptr first_exception() const {
    std::lock_guard<std::mutex> lk(exc_mtx_);
    return first_exc_;
  }
  std::int64_t pending() const { return pending_.load(std::memory_order_relaxed); }

  PoolStats stats() const {
    PoolStats s;
    s.per_worker.resize(n_);
    return s;
  }

 private:
  const unsigned n_;
  std::atomic<bool> done_{false};
  std::atomic<std::int64_t> pending_{0};

  std::mutex futs_mtx_;
  std::deque<std::future<void>> futs_;

  std::atomic<std::uint64_t> exceptions_{0};
  mutable std::mutex exc_mtx_;
  std::exception_ptr first_exc_;
};

}  // namespace taskforge
