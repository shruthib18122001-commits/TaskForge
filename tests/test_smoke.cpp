// Stage 1 smoke test: proves the CMake tree configures, GoogleTest links, and
// the sanitizer flags are wired through. Replaced in substance by the Stage 2+
// suites; kept as a canary.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

TEST(Smoke, GoogleTestLinks) {
  EXPECT_EQ(2 + 2, 4);
}

TEST(Smoke, ThreadsAndAtomicsWork) {
  std::atomic<int> counter{0};
  std::thread t([&] {
    for (int i = 0; i < 1000; ++i) {
      counter.fetch_add(1, std::memory_order_relaxed);
    }
  });
  for (int i = 0; i < 1000; ++i) {
    counter.fetch_add(1, std::memory_order_relaxed);
  }
  t.join();
  EXPECT_EQ(counter.load(), 2000);
}
