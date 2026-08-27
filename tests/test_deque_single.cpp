// Stage 2 -- ChaseLevDeque single-threaded semantics.
//
// The deque must be correct in isolation before it is trusted under
// concurrency. Owner sees LIFO (push/pop at bottom); steal() sees the opposite
// end; growth preserves order and every element.

#include "taskforge/chase_lev_deque.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using taskforge::ChaseLevDeque;
using taskforge::StealStatus;

TEST(DequeSingle, EmptyOnConstruction) {
  ChaseLevDeque<std::int64_t> d(64);
  EXPECT_TRUE(d.empty());
  EXPECT_EQ(d.size(), 0);
  EXPECT_FALSE(d.pop().has_value());

  std::int64_t out = -1;
  EXPECT_EQ(d.steal(out), StealStatus::kEmpty);
  EXPECT_FALSE(d.steal().has_value());
}

TEST(DequeSingle, CapacityRoundsUpToPow2WithFloor) {
  EXPECT_EQ(ChaseLevDeque<int>(1).capacity(), 64);    // floor
  EXPECT_EQ(ChaseLevDeque<int>(64).capacity(), 64);
  EXPECT_EQ(ChaseLevDeque<int>(65).capacity(), 128);
  EXPECT_EQ(ChaseLevDeque<int>(1000).capacity(), 1024);
}

TEST(DequeSingle, OwnerSeesLifo) {
  ChaseLevDeque<int> d(64);
  for (int i = 0; i < 32; ++i) d.push(i);
  EXPECT_EQ(d.size(), 32);
  for (int i = 31; i >= 0; --i) {
    auto v = d.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, i);
  }
  EXPECT_TRUE(d.empty());
  EXPECT_FALSE(d.pop().has_value());
}

TEST(DequeSingle, ThiefSeesOldestFirst) {
  ChaseLevDeque<int> d(64);
  for (int i = 0; i < 20; ++i) d.push(i);

  for (int i = 0; i < 12; ++i) {
    std::int64_t out = -1;
    ASSERT_EQ(d.steal(out), StealStatus::kSuccess);
    EXPECT_EQ(out, i);
  }
  // remaining 12..19 still LIFO from the owner end
  for (int i = 19; i >= 12; --i) {
    auto v = d.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, i);
  }
  EXPECT_TRUE(d.empty());
}

TEST(DequeSingle, LastElementGoesToExactlyOneCaller) {
  {
    ChaseLevDeque<int> d(64);
    d.push(7);
    auto v = d.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 7);
    std::int64_t out = -1;
    EXPECT_EQ(d.steal(out), StealStatus::kEmpty);
  }
  {
    ChaseLevDeque<int> d(64);
    d.push(7);
    std::int64_t out = -1;
    ASSERT_EQ(d.steal(out), StealStatus::kSuccess);
    EXPECT_EQ(out, 7);
    EXPECT_FALSE(d.pop().has_value());
    EXPECT_EQ(d.size(), 0);
  }
}

TEST(DequeSingle, InterleavedPushPop) {
  ChaseLevDeque<int> d(64);
  d.push(1);
  d.push(2);
  EXPECT_EQ(*d.pop(), 2);
  d.push(3);
  d.push(4);
  EXPECT_EQ(*d.pop(), 4);
  EXPECT_EQ(*d.pop(), 3);
  EXPECT_EQ(*d.pop(), 1);
  EXPECT_FALSE(d.pop().has_value());
}

TEST(DequeSingle, GrowthPreservesOrderAndCount) {
  ChaseLevDeque<std::int64_t> d(64);
  const std::int64_t n = 50000;
  for (std::int64_t i = 0; i < n; ++i) d.push(i);

  EXPECT_EQ(d.size(), n);
  EXPECT_GE(d.capacity(), n);
  EXPECT_GT(d.grow_count(), 0u);

  // steal from the front, pop from the back, meet in the middle.
  std::int64_t lo = 0, hi = n - 1;
  while (lo <= hi) {
    std::int64_t out = -1;
    ASSERT_EQ(d.steal(out), StealStatus::kSuccess);
    EXPECT_EQ(out, lo++);
    if (lo > hi) break;
    auto v = d.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, hi--);
  }
  EXPECT_TRUE(d.empty());
}

TEST(DequeSingle, PointerElementType) {
  // The pool instantiates ChaseLevDeque<Task*>.
  ChaseLevDeque<int*> d(64);
  int a = 10, b = 20, c = 30;
  d.push(&a);
  d.push(&b);
  d.push(&c);

  int* p = d.pop().value();
  EXPECT_EQ(p, &c);

  int* out = nullptr;
  ASSERT_EQ(d.steal(out), StealStatus::kSuccess);
  EXPECT_EQ(out, &a);

  EXPECT_EQ(d.pop().value(), &b);
  EXPECT_TRUE(d.empty());
}

TEST(DequeSingle, ManyGrowShrinkCycles) {
  ChaseLevDeque<int> d(64);
  for (int cycle = 0; cycle < 100; ++cycle) {
    for (int i = 0; i < 500; ++i) d.push(i);
    for (int i = 499; i >= 0; --i) {
      auto v = d.pop();
      ASSERT_TRUE(v.has_value());
      EXPECT_EQ(*v, i);
    }
    EXPECT_TRUE(d.empty());
  }
}
