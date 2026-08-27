// task.hpp -- the unit of work every pool schedules.
//
// A heap-allocated Task holds a type-erased nullary callable. The three pools
// (work-stealing, mutex-queue, std::async) all allocate and free Task the same
// way, so a benchmark comparing them measures *scheduling*, not who has the
// cheaper task object.
//
// Ownership protocol (work-stealing pool):
//   * submit() does `new Task`, hands the raw pointer to a deque / the
//     injection queue, and bumps pending_.
//   * Whichever worker extracts the pointer wraps it in unique_ptr, runs it,
//     and lets the unique_ptr free it -- exactly one worker ever runs a Task.
//   * On shutdown-now, undrained Task pointers are deleted without running.
//
// std::function is used for type erasure. For sub-microsecond tasks its heap
// allocation is a measurable fraction of the cost; DESIGN.md's "what didn't
// work / could improve" notes a small-buffer callable as the follow-up. It is
// applied uniformly across pools so comparisons stay fair.

#pragma once

#include <functional>
#include <utility>

namespace taskforge {

struct Task {
  std::function<void()> run;

  Task() = default;
  explicit Task(std::function<void()> f) : run(std::move(f)) {}
};

}  // namespace taskforge
