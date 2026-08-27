// verify_deque_single.cpp -- single-threaded semantics of ChaseLevDeque.
// Owner sees LIFO via push/pop; steal() sees the opposite (oldest) end;
// growth preserves ordering and every element.

#include "taskforge/chase_lev_deque.hpp"

#include <cstdint>
#include <optional>
#include <vector>

#include "../verify/tf_verify.hpp"

using taskforge::ChaseLevDeque;
using taskforge::StealStatus;

static void empty_deque_behaviour() {
  ChaseLevDeque<std::int64_t> d(64);
  TF_CHECK(d.empty());
  TF_CHECK_EQ(d.size(), std::int64_t{0});
  TF_CHECK(!d.pop().has_value());

  std::int64_t out = -1;
  TF_CHECK(d.steal(out) == StealStatus::kEmpty);
  TF_CHECK(!d.steal().has_value());
}

static void lifo_for_owner() {
  ChaseLevDeque<std::int64_t> d(64);
  for (std::int64_t i = 0; i < 10; ++i) d.push(i);
  TF_CHECK_EQ(d.size(), std::int64_t{10});
  for (std::int64_t i = 9; i >= 0; --i) {
    auto v = d.pop();
    TF_CHECK(v.has_value());
    TF_CHECK_EQ(*v, i);
  }
  TF_CHECK(d.empty());
  TF_CHECK(!d.pop().has_value());
}

static void steal_takes_the_oldest() {
  ChaseLevDeque<std::int64_t> d(64);
  for (std::int64_t i = 0; i < 10; ++i) d.push(i);
  // Thief end is FIFO w.r.t. push order: 0,1,2,...
  for (std::int64_t i = 0; i < 5; ++i) {
    std::int64_t out = -1;
    TF_CHECK(d.steal(out) == StealStatus::kSuccess);
    TF_CHECK_EQ(out, i);
  }
  // Owner end still LIFO on what remains: 9,8,7,6,5
  for (std::int64_t i = 9; i >= 5; --i) {
    auto v = d.pop();
    TF_CHECK(v.has_value());
    TF_CHECK_EQ(*v, i);
  }
  TF_CHECK(d.empty());
}

static void pop_then_steal_last_element_is_exclusive() {
  // With one element, exactly one of pop()/steal() may return it. Single
  // thread, so no race -- just check the bookkeeping both ways round.
  {
    ChaseLevDeque<std::int64_t> d(64);
    d.push(42);
    auto v = d.pop();
    TF_CHECK(v.has_value() && *v == 42);
    std::int64_t out = -1;
    TF_CHECK(d.steal(out) == StealStatus::kEmpty);
  }
  {
    ChaseLevDeque<std::int64_t> d(64);
    d.push(42);
    std::int64_t out = -1;
    TF_CHECK(d.steal(out) == StealStatus::kSuccess && out == 42);
    TF_CHECK(!d.pop().has_value());
    TF_CHECK_EQ(d.size(), std::int64_t{0});
  }
}

static void growth_preserves_everything() {
  ChaseLevDeque<std::int64_t> d(64);  // rounds to 64
  const std::int64_t n = 100000;      // forces many doublings
  for (std::int64_t i = 0; i < n; ++i) d.push(i);
  TF_CHECK_EQ(d.size(), n);
  TF_CHECK(d.capacity() >= n);
  TF_CHECK(d.grow_count() > 0);

  // Interleave steals (oldest) and pops (newest) and check they meet in the
  // middle with no gaps or repeats.
  std::int64_t lo = 0, hi = n - 1;
  while (lo <= hi) {
    std::int64_t out = -1;
    TF_CHECK(d.steal(out) == StealStatus::kSuccess);
    TF_CHECK_EQ(out, lo);
    ++lo;
    if (lo > hi) break;
    auto v = d.pop();
    TF_CHECK(v.has_value());
    TF_CHECK_EQ(*v, hi);
    --hi;
  }
  TF_CHECK(d.empty());
}

static void pointer_element_type() {
  // The pool uses ChaseLevDeque<Task*>; make sure a pointer T works.
  ChaseLevDeque<int*> d(64);
  int a = 1, b = 2, c = 3;
  d.push(&a); d.push(&b); d.push(&c);
  TF_CHECK_EQ(*d.pop().value(), 3);
  int* out = nullptr;
  TF_CHECK(d.steal(out) == StealStatus::kSuccess && out == &a);
  TF_CHECK_EQ(*d.pop().value(), 2);
}

int main() {
  empty_deque_behaviour();
  lifo_for_owner();
  steal_takes_the_oldest();
  pop_then_steal_last_element_is_exclusive();
  growth_preserves_everything();
  pointer_element_type();
  TF_PASS("verify_deque_single");
}
