// verify_dag.cpp -- the DAG resolver: dependency order is respected, cycles are
// rejected at submission, disjoint components all run, a graph is re-runnable,
// and a throwing node is reported without stalling the run.

#include "taskforge/dag.hpp"
#include "taskforge/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../verify/tf_verify.hpp"

using taskforge::CycleError;
using taskforge::Graph;
using taskforge::ThreadPool;
using clk = std::chrono::steady_clock;

// Records, per node, the interval [start, end] it ran in. After a run we check
// every edge u->v has end[u] <= start[v]: v never started before u finished.
struct Timeline {
  std::vector<std::atomic<long long>> start, end;
  explicit Timeline(std::size_t n) : start(n), end(n) {}
  auto stamp() { return std::chrono::duration_cast<std::chrono::nanoseconds>(
                     clk::now().time_since_epoch()).count(); }
};

static void chain_runs_in_order() {
  ThreadPool pool(4);
  const int n = 200;
  Graph g;
  Timeline tl(static_cast<std::size_t>(n));
  std::vector<Graph::NodeId> ids;
  for (int i = 0; i < n; ++i) {
    std::vector<Graph::NodeId> pre;
    if (i > 0) pre.push_back(ids.back());
    ids.push_back(g.add_node(
        [i, &tl] {
          tl.start[static_cast<std::size_t>(i)].store(tl.stamp());
          std::this_thread::sleep_for(std::chrono::microseconds(50));
          tl.end[static_cast<std::size_t>(i)].store(tl.stamp());
        },
        pre));
  }
  g.run(pool);
  for (int i = 1; i < n; ++i)
    TF_CHECK_MSG(tl.end[static_cast<std::size_t>(i - 1)].load() <=
                     tl.start[static_cast<std::size_t>(i)].load(),
                 "chain node " + std::to_string(i) + " started before " +
                     std::to_string(i - 1) + " finished");
}

static void diamond_and_wide_fan() {
  ThreadPool pool(6);
  // 0 -> {1..K} -> K+1
  const int K = 64;
  Graph g;
  Timeline tl(static_cast<std::size_t>(K + 2));
  auto body = [&tl](int i) {
    return [i, &tl] {
      tl.start[static_cast<std::size_t>(i)].store(tl.stamp());
      tl.end[static_cast<std::size_t>(i)].store(tl.stamp());
    };
  };
  auto root = g.add_node(body(0));
  std::vector<Graph::NodeId> mids;
  for (int i = 1; i <= K; ++i) mids.push_back(g.add_node(body(i), {root}));
  auto sink = g.add_node(body(K + 1), mids);

  g.run(pool);
  const long long r_end = tl.end[0].load();
  const long long s_start = tl.start[static_cast<std::size_t>(K + 1)].load();
  for (int i = 1; i <= K; ++i) {
    TF_CHECK(tl.start[static_cast<std::size_t>(i)].load() >= r_end);
    TF_CHECK(tl.end[static_cast<std::size_t>(i)].load() <= s_start);
  }
  (void)sink;
}

static void disjoint_components_all_run() {
  ThreadPool pool(4);
  Graph g;
  std::atomic<int> ran{0};
  // three independent chains
  for (int c = 0; c < 3; ++c) {
    Graph::NodeId prev = g.add_node([&ran] { ran.fetch_add(1); });
    for (int i = 0; i < 10; ++i)
      prev = g.add_node([&ran] { ran.fetch_add(1); }, {prev});
  }
  g.run(pool);
  TF_CHECK_EQ(ran.load(), 33);
}

static void reusable_across_runs() {
  ThreadPool pool(4);
  Graph g;
  std::atomic<int> ran{0};
  auto a = g.add_node([&ran] { ran.fetch_add(1); });
  auto b = g.add_node([&ran] { ran.fetch_add(1); }, {a});
  g.add_node([&ran] { ran.fetch_add(1); }, {b});
  g.run(pool);
  g.run(pool);
  g.run(pool);
  TF_CHECK_EQ(ran.load(), 9);
}

static void cycles_are_rejected() {
  ThreadPool pool(2);

  // Out-of-range edge endpoint.
  {
    Graph g;
    g.add_node([] {});
    bool threw = false;
    try {
      g.add_edge(0, 9);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    TF_CHECK(threw);
  }

  auto is_cycle = [&](Graph& g) {
    if (g.is_acyclic()) return false;
    try {
      g.run(pool);
    } catch (const CycleError&) {
      return true;
    }
    return false;
  };

  // Self-loop.
  {
    Graph g;
    auto a = g.add_node([] {});
    g.add_edge(a, a);
    TF_CHECK(is_cycle(g));
  }
  // 2-cycle.
  {
    Graph g;
    auto a = g.add_node([] {});
    auto b = g.add_node([] {});
    g.add_edge(a, b);
    g.add_edge(b, a);
    TF_CHECK(is_cycle(g));
  }
  // Longer cycle buried in an otherwise-fine DAG.
  {
    Graph g;
    std::vector<Graph::NodeId> id;
    for (int i = 0; i < 12; ++i) id.push_back(g.add_node([] {}));
    for (int i = 0; i < 8; ++i) g.add_edge(id[static_cast<std::size_t>(i)],
                                           id[static_cast<std::size_t>(i + 1)]);
    // close 9 -> 10 -> 11 -> 9
    g.add_edge(id[9], id[10]);
    g.add_edge(id[10], id[11]);
    g.add_edge(id[11], id[9]);
    TF_CHECK(is_cycle(g));
  }
  // A genuine DAG is NOT falsely flagged, and is re-runnable.
  {
    Graph g;
    auto n0 = g.add_node([] {});
    auto n1 = g.add_node([] {}, {n0});
    auto n2 = g.add_node([] {}, {n1});
    g.add_edge(n0, n2);  // extra forward edge, still acyclic
    TF_CHECK(g.is_acyclic());
    TF_CHECK_EQ(g.topo_order().size(), std::size_t{3});
    g.run(pool);
  }
}

static void throwing_node_is_reported_not_hung() {
  ThreadPool pool(4);
  Graph g;
  std::atomic<int> ran{0};
  auto a = g.add_node([&ran] { ran.fetch_add(1); });
  auto bad = g.add_node([] { throw std::runtime_error("node blew up"); }, {a});
  g.add_node([&ran] { ran.fetch_add(1); }, {bad});  // still runs (documented policy)

  bool threw = false;
  try {
    g.run(pool);
  } catch (const std::runtime_error& e) {
    threw = std::string(e.what()) == "node blew up";
  }
  TF_CHECK(threw);
  TF_CHECK_EQ(ran.load(), 2);  // predecessor and successor both ran
}

int main() {
  chain_runs_in_order();
  diamond_and_wide_fan();
  disjoint_components_all_run();
  reusable_across_runs();
  cycles_are_rejected();
  throwing_node_is_reported_not_hung();
  TF_PASS("verify_dag");
}
