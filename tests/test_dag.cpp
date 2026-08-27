// Stage 6 -- DAG resolver: dependency order, fan-out/in, disjoint components,
// re-runnability, and results visibility across edges.

#include "taskforge/dag.hpp"
#include "taskforge/thread_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using taskforge::Graph;
using taskforge::ThreadPool;
using clk = std::chrono::steady_clock;

namespace {
long long now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             clk::now().time_since_epoch())
      .count();
}
}  // namespace

TEST(Dag, LinearChainRunsInOrder) {
  ThreadPool pool(4);
  const int n = 150;
  Graph g;
  std::vector<std::atomic<long long>> start(n), end(n);
  std::vector<Graph::NodeId> id;
  for (int i = 0; i < n; ++i) {
    std::vector<Graph::NodeId> pre;
    if (i > 0) pre.push_back(id.back());
    id.push_back(g.add_node(
        [i, &start, &end] {
          start[static_cast<std::size_t>(i)].store(now_ns());
          std::this_thread::sleep_for(std::chrono::microseconds(30));
          end[static_cast<std::size_t>(i)].store(now_ns());
        },
        pre));
  }
  g.run(pool);
  for (int i = 1; i < n; ++i)
    EXPECT_LE(end[static_cast<std::size_t>(i - 1)].load(),
              start[static_cast<std::size_t>(i)].load())
        << "node " << i << " started before " << (i - 1) << " finished";
}

TEST(Dag, DiamondFanOutFanIn) {
  ThreadPool pool(6);
  const int K = 50;
  Graph g;
  std::atomic<int> mids_running{0};
  std::atomic<int> max_parallel{0};
  std::atomic<bool> root_done{false};
  std::atomic<bool> sink_started{false};

  auto root = g.add_node([&] { root_done.store(true); });
  std::vector<Graph::NodeId> mids;
  for (int i = 0; i < K; ++i) {
    mids.push_back(g.add_node(
        [&] {
          EXPECT_TRUE(root_done.load());
          EXPECT_FALSE(sink_started.load());
          int cur = mids_running.fetch_add(1) + 1;
          int prev = max_parallel.load();
          while (cur > prev && !max_parallel.compare_exchange_weak(prev, cur)) {
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          mids_running.fetch_sub(1);
        },
        {root}));
  }
  g.add_node([&] { sink_started.store(true); }, mids);

  g.run(pool);
  EXPECT_GT(max_parallel.load(), 1) << "middle layer should have run in parallel";
}

TEST(Dag, DisjointComponentsAllRun) {
  ThreadPool pool(4);
  Graph g;
  std::atomic<int> ran{0};
  for (int c = 0; c < 5; ++c) {
    Graph::NodeId prev = g.add_node([&ran] { ran.fetch_add(1); });
    for (int i = 0; i < 8; ++i)
      prev = g.add_node([&ran] { ran.fetch_add(1); }, {prev});
  }
  g.run(pool);
  EXPECT_EQ(ran.load(), 5 * 9);
}

TEST(Dag, ReRunnable) {
  ThreadPool pool(4);
  Graph g;
  std::atomic<int> ran{0};
  auto a = g.add_node([&ran] { ran.fetch_add(1); });
  auto b = g.add_node([&ran] { ran.fetch_add(1); }, {a});
  g.add_node([&ran] { ran.fetch_add(1); }, {b});
  for (int r = 0; r < 4; ++r) g.run(pool);
  EXPECT_EQ(ran.load(), 12);
}

TEST(Dag, PredecessorResultsVisibleToSuccessor) {
  // Each node writes to its own slot; a successor reads all predecessors'
  // slots. The acq_rel on the dependency counter must make those writes
  // visible. Run it many times to shake out a missing edge.
  ThreadPool pool(8);
  for (int rep = 0; rep < 50; ++rep) {
    Graph g;
    const int W = 16;
    std::vector<int> slot(static_cast<std::size_t>(W + 1), 0);
    std::vector<Graph::NodeId> mids;
    auto root = g.add_node([] {});
    for (int i = 0; i < W; ++i)
      mids.push_back(g.add_node([i, &slot] { slot[static_cast<std::size_t>(i)] = i + 1; },
                                {root}));
    int sink_sum = -1;
    g.add_node(
        [&] {
          int s = 0;
          for (int i = 0; i < W; ++i) s += slot[static_cast<std::size_t>(i)];
          sink_sum = s;
        },
        mids);
    g.run(pool);
    EXPECT_EQ(sink_sum, W * (W + 1) / 2) << "rep " << rep;
  }
}

TEST(Dag, EmptyGraphIsNoop) {
  ThreadPool pool(2);
  Graph g;
  g.run(pool);  // must not hang or throw
  SUCCEED();
}

TEST(Dag, TopoOrderRespectsEdges) {
  Graph g;
  auto a = g.add_node([] {});
  auto b = g.add_node([] {}, {a});
  auto c = g.add_node([] {}, {a});
  auto d = g.add_node([] {}, {b, c});
  auto order = g.topo_order();
  ASSERT_EQ(order.size(), 4u);
  auto pos = [&](Graph::NodeId n) {
    return std::find(order.begin(), order.end(), n) - order.begin();
  };
  EXPECT_LT(pos(a), pos(b));
  EXPECT_LT(pos(a), pos(c));
  EXPECT_LT(pos(b), pos(d));
  EXPECT_LT(pos(c), pos(d));
}
