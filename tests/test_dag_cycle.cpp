// Stage 6 -- cycle detection. Every cycle shape is rejected at run() with a
// CycleError, before any task is submitted; valid DAGs are never falsely
// flagged; bad edge endpoints are rejected at add_edge().

#include "taskforge/dag.hpp"
#include "taskforge/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <vector>

using taskforge::CycleError;
using taskforge::Graph;
using taskforge::ThreadPool;

TEST(DagCycle, EdgeEndpointOutOfRangeThrows) {
  Graph g;
  g.add_node([] {});
  EXPECT_THROW(g.add_edge(0, 7), std::invalid_argument);
  EXPECT_THROW(g.add_edge(7, 0), std::invalid_argument);
}

TEST(DagCycle, SelfLoop) {
  Graph g;
  auto a = g.add_node([] {});
  g.add_edge(a, a);
  EXPECT_FALSE(g.is_acyclic());
  EXPECT_TRUE(g.topo_order().empty());
  ThreadPool pool(2);
  EXPECT_THROW(g.run(pool), CycleError);
}

TEST(DagCycle, TwoCycle) {
  Graph g;
  auto a = g.add_node([] {});
  auto b = g.add_node([] {});
  g.add_edge(a, b);
  g.add_edge(b, a);
  ThreadPool pool(2);
  EXPECT_THROW(g.run(pool), CycleError);
}

TEST(DagCycle, LongCycleBuriedInDag) {
  Graph g;
  std::vector<Graph::NodeId> id;
  for (int i = 0; i < 20; ++i) id.push_back(g.add_node([] {}));
  for (int i = 0; i < 15; ++i)
    g.add_edge(id[static_cast<std::size_t>(i)], id[static_cast<std::size_t>(i + 1)]);
  // close 16 -> 17 -> 18 -> 19 -> 16
  g.add_edge(id[16], id[17]);
  g.add_edge(id[17], id[18]);
  g.add_edge(id[18], id[19]);
  g.add_edge(id[19], id[16]);

  EXPECT_FALSE(g.is_acyclic());
  ThreadPool pool(4);
  EXPECT_THROW(g.run(pool), CycleError);
}

TEST(DagCycle, NoTaskRunsWhenCycleRejected) {
  Graph g;
  std::atomic<int> ran{0};
  auto a = g.add_node([&ran] { ran.fetch_add(1); });
  auto b = g.add_node([&ran] { ran.fetch_add(1); });
  auto c = g.add_node([&ran] { ran.fetch_add(1); });
  g.add_edge(a, b);
  g.add_edge(b, c);
  g.add_edge(c, a);  // cycle

  ThreadPool pool(4);
  EXPECT_THROW(g.run(pool), CycleError);
  EXPECT_EQ(ran.load(), 0) << "cycle must be caught before anything is submitted";
}

TEST(DagCycle, ValidDagNotFalselyFlagged) {
  Graph g;
  auto a = g.add_node([] {});
  auto b = g.add_node([] {}, {a});
  auto c = g.add_node([] {}, {a});
  auto d = g.add_node([] {}, {b, c});
  g.add_edge(a, d);  // redundant but still acyclic
  EXPECT_TRUE(g.is_acyclic());
  EXPECT_EQ(g.topo_order().size(), 4u);
  ThreadPool pool(4);
  EXPECT_NO_THROW(g.run(pool));
}

TEST(DagCycle, DiamondIsAcyclic) {
  Graph g;
  auto top = g.add_node([] {});
  auto l = g.add_node([] {}, {top});
  auto r = g.add_node([] {}, {top});
  g.add_node([] {}, {l, r});
  EXPECT_TRUE(g.is_acyclic());
}
