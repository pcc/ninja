// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NINJA_STATE_H_
#define NINJA_STATE_H_

#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <set>
#include <string>
#include <vector>

#include "disk_interface.h"
#include "eval_env.h"
#include "graph.h"
#include "hash_map.h"
#include "util.h"

struct Edge;
struct Node;
struct Rule;

/// A pool for delayed edges.
/// Pools are scoped to a State. Edges within a State will share Pools. A Pool
/// will keep a count of the total 'weight' of the currently scheduled edges. If
/// a Plan attempts to schedule an Edge which would cause the total weight to
/// exceed the depth of the Pool, the Pool will enqueue the Edge instead of
/// allowing the Plan to schedule it. The Pool will relinquish queued Edges when
/// the total scheduled weight diminishes enough (i.e. when a scheduled edge
/// completes).
struct Pool {
  Pool(const std::string& name, int depth)
    : name_(name), current_use_(0), depth_(depth), delayed_() {}

  // A depth of 0 is infinite
  bool is_valid() const { return depth_ >= 0; }
  int depth() const { return depth_; }
  const std::string& name() const { return name_; }
  int current_use() const { return current_use_; }

  /// true if the Pool might delay this edge
  bool ShouldDelayEdge() const { return depth_ != 0; }

  /// informs this Pool that the given edge is committed to be run.
  /// Pool will count this edge as using resources from this pool.
  void EdgeScheduled(const Edge& edge);

  /// informs this Pool that the given edge is no longer runnable, and should
  /// relinquish its resources back to the pool
  void EdgeFinished(const Edge& edge);

  /// adds the given edge to this Pool to be delayed.
  void DelayEdge(Edge* edge);

  /// Pool will add zero or more edges to the ready_queue
  void RetrieveReadyEdges(EdgePriorityQueue* ready_queue);

  /// Dump the Pool and its edges (useful for debugging).
  void Dump() const;

 private:
  std::string name_;

  /// |current_use_| is the total of the weights of the edges which are
  /// currently scheduled in the Plan (i.e. the edges in Plan::ready_).
  int current_use_;
  int depth_;

  struct WeightedEdgeCmp {
    bool operator()(const Edge* a, const Edge* b) const {
      if (!a) return b;
      if (!b) return false;
      int weight_diff = a->weight() - b->weight();
      if (weight_diff != 0) {
        return weight_diff < 0;
      }
      return EdgePriorityGreater()(a, b);
    }
  };

  typedef std::set<Edge*, WeightedEdgeCmp> DelayedEdges;
  DelayedEdges delayed_;
};

/// The mapping of paths to nodes.  Lookups and insertions may happen
/// concurrently: the map is sharded by hash, with a lock per shard.
struct PathMap {
  /// Return the node for \a path, or null.
  Node* Lookup(StringPiece path) const;

  /// Return the node for \a path, creating it with \a slash_bits if there
  /// is none.  See Node::slash_bits_provisional() for \a provisional.
  Node* GetOrCreate(StringPiece path, uint64_t slash_bits,
                    bool provisional = false) {
    return GetOrCreate(path, Hash(path), slash_bits, provisional);
  }
  /// Like the above, with the Hash() of \a path.
  Node* GetOrCreate(StringPiece path, uint64_t hash, uint64_t slash_bits,
                    bool provisional);

  static uint64_t Hash(StringPiece path);

  /// Remove the node for \a path (which is not freed).
  void Erase(StringPiece path);

  size_t size() const;
  size_t bucket_count() const;

  /// Call \a f for all nodes, in an unspecified order (but not
  /// concurrently with insertions).
  template <typename F>
  void ForEach(F f) const {
    for (const Shard& shard : shards_)
      for (const auto& entry : shard.map)
        f(entry.second);
  }

 private:
  struct Key {
    StringPiece path;
    uint64_t hash;
    bool operator==(const Key& other) const { return path == other.path; }
  };
  struct KeyHash {
    size_t operator()(const Key& key) const { return key.hash; }
  };
  static const int kShardBits = 6;
  /// A spin lock: the critical sections are short.  (Yield after a while,
  /// in case the thread holding it doesn't run.)
  struct SpinLock {
    void lock() {
      int spins = 0;
      while (locked_.exchange(true, std::memory_order_acquire)) {
        while (locked_.load(std::memory_order_relaxed)) {
          if (++spins > 1000)
            std::this_thread::yield();
        }
      }
    }
    void unlock() { locked_.store(false, std::memory_order_release); }

   private:
    std::atomic<bool> locked_{false};
  };
  struct alignas(64) Shard {
    mutable SpinLock mutex;
    emhash8::HashMap<Key, Node*, KeyHash> map;
  };
  const Shard& ShardFor(uint64_t hash) const {
    return shards_[hash >> (64 - kShardBits)];
  }
  Shard& ShardFor(uint64_t hash) {
    return shards_[hash >> (64 - kShardBits)];
  }

  Shard shards_[1 << kShardBits];
};

/// Global state (file status) for a single run.
struct State {
  static Pool kDefaultPool;
  static Pool kConsolePool;

  State();

  void AddPool(Pool* pool);
  Pool* LookupPool(const std::string& pool_name);

  Edge* AddEdge(const Rule* rule);

  /// Create an edge (on any thread) to be added with AppendEdge() later.
  static Edge* CreateEdge(const Rule* rule, BindingEnv* env);
  void AppendEdge(Edge* edge);

  Node* GetNode(StringPiece path, uint64_t slash_bits);
  Node* LookupNode(StringPiece path) const;
  Node* SpellcheckNode(const std::string& path);

  /// Add input / output / validation nodes to a given edge. This also
  /// ensures that the generated_by_dep_loader() flag for all these nodes
  /// is set to false, to indicate that they come from the input manifest.
  void AddIn(Edge* edge, StringPiece path, uint64_t slash_bits);
  bool AddOut(Edge* edge, StringPiece path, uint64_t slash_bits, std::string* err);
  void AddValidation(Edge* edge, StringPiece path, uint64_t slash_bits);

  /// Like the above, for a node (for \a path) that was already looked up.
  void AddIn(Edge* edge, Node* node, uint64_t slash_bits);
  bool AddOut(Edge* edge, Node* node, uint64_t slash_bits, std::string* err);
  void AddValidation(Edge* edge, Node* node, uint64_t slash_bits);

  /// Like the above, for nodes that are already in the edge's inputs,
  /// outputs or validations.
  void ConnectInput(Edge* edge, Node* node, uint64_t slash_bits);
  bool ConnectOutput(Edge* edge, Node* node, uint64_t slash_bits,
                     std::string* err);
  void ConnectValidation(Edge* edge, Node* node, uint64_t slash_bits);
  bool AddDefault(StringPiece path, std::string* error);

  /// Reset state.  Keeps all nodes and edges, but restores them to the
  /// state where we haven't yet examined the disk for dirty state.
  void Reset();

  /// Dump the nodes and Pools (useful for debugging).
  void Dump();

  /// @return the root node(s) of the graph. (Root nodes have no output edges).
  /// @param error where to write the error message if somethings went wrong.
  std::vector<Node*> RootNodes(std::string* error) const;
  std::vector<Node*> DefaultNodes(std::string* error) const;

  /// Mapping of path -> Node.
  PathMap paths_;

  /// All the pools used in the graph.
  std::map<std::string, Pool*> pools_;

  /// All the edges of the graph.
  std::vector<Edge*> edges_;

  BindingEnv bindings_;
  std::vector<Node*> defaults_;

  /// The contents of the manifest files, which bindings point into.
  std::vector<std::unique_ptr<FileContents> > manifest_contents_;
};

#endif  // NINJA_STATE_H_
