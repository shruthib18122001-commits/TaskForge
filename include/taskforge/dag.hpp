// dag.hpp
// =============================================================================
// A DAG dependency resolver on top of any of the pools (ThreadPool /
// MutexQueuePool -- anything with submit() + wait_idle()).
//
// Build the graph single-threaded:
//     NodeId a = g.add_node(fnA);
//     NodeId b = g.add_node(fnB, {a});      // b depends on a
//     g.add_edge(a, c);                     // or add edges after the fact
//
// Edges may be added in any order, including ones that close a cycle -- the
// cycle is caught at run() time, not hidden by the builder.
//
// run(pool):
//   1. Cycle check up front by Kahn topological sort over the declared edges.
//      The whole graph is known before execution, so this is a cheap O(V+E)
//      pass and a cycle is rejected before a single task is submitted.
//   2. Every node with zero prerequisites is submitted to the pool.
//   3. When a node's task finishes it decrements each successor's
//      unresolved-count; whichever decrement hits zero submits that successor.
//   4. run() returns when every node has executed (pool.wait_idle()).
//
// The memory ordering of the dependency counter is the interesting part and is
// spelled out at the fetch_sub below and in DESIGN.md.
// =============================================================================

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

namespace taskforge {

class CycleError : public std::runtime_error {
 public:
  explicit CycleError(const std::string& what) : std::runtime_error(what) {}
};

class Graph {
 public:
  using NodeId = std::size_t;

  // Add a node with no edges yet.
  template <class F>
  NodeId add_node(F&& fn) {
    const NodeId id = nodes_.size();
    auto n = std::make_unique<Node>();
    n->fn = std::forward<F>(fn);
    nodes_.push_back(std::move(n));
    return id;
  }

  // Convenience: add a node that depends on `prerequisites` (which must already
  // exist). Additional edges -- including cycle-closing ones -- can still be
  // added afterwards with add_edge().
  template <class F>
  NodeId add_node(F&& fn, const std::vector<NodeId>& prerequisites) {
    const NodeId id = add_node(std::forward<F>(fn));
    for (NodeId p : prerequisites) add_edge(p, id);
    return id;
  }

  // Declare that `to` depends on `from` (from must run first). Both ids must
  // already exist. No cycle checking here -- run()/is_acyclic() does that.
  void add_edge(NodeId from, NodeId to) {
    check_id(from);
    check_id(to);
    nodes_[from]->successors.push_back(to);
    ++nodes_[to]->static_deps;
  }

  std::size_t size() const { return nodes_.size(); }

  bool is_acyclic() const { return topo_order().size() == nodes_.size(); }

  // A valid topological order, or an empty vector if the graph has a cycle.
  std::vector<NodeId> topo_order() const {
    std::vector<std::size_t> indeg(nodes_.size());
    for (std::size_t i = 0; i < nodes_.size(); ++i)
      indeg[i] = nodes_[i]->static_deps;

    std::queue<NodeId> q;
    for (std::size_t i = 0; i < nodes_.size(); ++i)
      if (indeg[i] == 0) q.push(i);

    std::vector<NodeId> order;
    order.reserve(nodes_.size());
    while (!q.empty()) {
      const NodeId u = q.front();
      q.pop();
      order.push_back(u);
      for (NodeId v : nodes_[u]->successors)
        if (--indeg[v] == 0) q.push(v);
    }
    if (order.size() != nodes_.size()) return {};  // cycle
    return order;
  }

  // Execute the graph on `pool` and block until every node has run.
  // Throws CycleError if the graph is not a DAG. Re-runnable.
  template <class Pool>
  void run(Pool& pool) {
    if (nodes_.empty()) return;
    const std::vector<NodeId> order = topo_order();
    if (order.size() != nodes_.size()) {
      throw CycleError("Graph::run: dependency cycle detected among " +
                       std::to_string(nodes_.size() - order.size()) + " of " +
                       std::to_string(nodes_.size()) + " nodes");
    }

    for (auto& n : nodes_)
      n->remaining.store(static_cast<std::int64_t>(n->static_deps),
                         std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> g(fail_mtx_);
      first_exc_ = nullptr;
    }

    // Seed roots. schedule() is the only thing that submits nodes.
    for (std::size_t i = 0; i < nodes_.size(); ++i)
      if (nodes_[i]->static_deps == 0) schedule(pool, i);

    pool.wait_idle();

    // Policy: a node that throws does not stall the DAG -- successors still run
    // (their inputs may be incomplete, so don't throw from a node unless you
    // mean it). The first exception is re-raised here so run() still reports it.
    std::exception_ptr e;
    {
      std::lock_guard<std::mutex> g(fail_mtx_);
      e = first_exc_;
    }
    if (e) std::rethrow_exception(e);
  }

 private:
  struct Node {
    std::function<void()> fn;
    std::size_t static_deps = 0;              // in-degree (declared)
    std::vector<NodeId> successors;
    std::atomic<std::int64_t> remaining{0};   // unresolved prerequisites, this run
  };

  void check_id(NodeId id) const {
    if (id >= nodes_.size())
      throw std::invalid_argument("Graph: node id " + std::to_string(id) +
                                  " out of range (" + std::to_string(nodes_.size()) +
                                  " nodes)");
  }

  template <class Pool>
  void schedule(Pool& pool, NodeId id) {
    pool.submit([this, &pool, id] {
      try {
        nodes_[id]->fn();
      } catch (...) {
        std::lock_guard<std::mutex> g(fail_mtx_);
        if (!first_exc_) first_exc_ = std::current_exception();
      }

      // Release each successor. The fetch_sub is acq_rel:
      //   * RELEASE: publishes everything this node's fn() wrote, so a
      //     successor that later runs sees it.
      //   * ACQUIRE: the decrement that observes zero must have seen the
      //     release-decrements of ALL other predecessors of that successor --
      //     otherwise the successor could run without seeing some
      //     predecessor's results.
      // The decrement whose previous value was 1 (zero after) is the one that
      // schedules the successor; submit()'s own release/acquire then carries
      // the happens-before to whichever worker picks the successor up.
      for (NodeId s : nodes_[id]->successors) {
        if (nodes_[s]->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          schedule(pool, s);
        }
      }
    });
  }

  std::vector<std::unique_ptr<Node>> nodes_;

  mutable std::mutex fail_mtx_;
  std::exception_ptr first_exc_;
};

}  // namespace taskforge
