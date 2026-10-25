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

#include "state.h"

#include <assert.h>
#include <stdio.h>

#include <cstddef>

#include "edit_distance.h"
#include "graph.h"
#include "util.h"

using namespace std;

void Pool::EdgeScheduled(const Edge& edge) {
  if (depth_ != 0)
    current_use_ += edge.weight();
}

void Pool::EdgeFinished(const Edge& edge) {
  if (depth_ != 0)
    current_use_ -= edge.weight();
}

void Pool::DelayEdge(Edge* edge) {
  assert(depth_ != 0);
  delayed_.insert(edge);
}

void Pool::RetrieveReadyEdges(EdgePriorityQueue* ready_queue) {
  DelayedEdges::iterator it = delayed_.begin();
  while (it != delayed_.end()) {
    Edge* edge = *it;
    if (current_use_ + edge->weight() > depth_)
      break;
    ready_queue->push(edge);
    EdgeScheduled(*edge);
    ++it;
  }
  delayed_.erase(delayed_.begin(), it);
}

void Pool::Dump() const {
  printf("%s (%d/%d) ->\n", name_.c_str(), current_use_, depth_);
  for (DelayedEdges::const_iterator it = delayed_.begin();
       it != delayed_.end(); ++it)
  {
    printf("\t");
    (*it)->Dump();
  }
}

namespace {

/// Allocate memory for an object that is never freed: nodes and edges live
/// as long as the process.  Allocating them in blocks per thread is faster.
void* AllocateForever(size_t size) {
  const size_t kBlockSize = 1 << 20;
  thread_local char* next = nullptr;
  thread_local size_t left = 0;
  size = (size + alignof(std::max_align_t) - 1) &
         ~(alignof(std::max_align_t) - 1);
  if (size > left) {
    if (size > kBlockSize / 8)
      return ::operator new(size);
    next = static_cast<char*>(::operator new(kBlockSize));
    left = kBlockSize;
  }
  void* result = next;
  next += size;
  left -= size;
  return result;
}

}  // namespace

Pool State::kDefaultPool("", 0);
Pool State::kConsolePool("console", 1);

State::State() {
  bindings_.AddRule(Rule::Phony());
  AddPool(&kDefaultPool);
  AddPool(&kConsolePool);
}

void State::AddPool(Pool* pool) {
  assert(LookupPool(pool->name()) == NULL);
  pools_[pool->name()] = pool;
}

Pool* State::LookupPool(const string& pool_name) {
  map<string, Pool*>::iterator i = pools_.find(pool_name);
  if (i == pools_.end())
    return NULL;
  return i->second;
}

Edge* State::AddEdge(const Rule* rule) {
  Edge* edge = CreateEdge(rule, &bindings_);
  AppendEdge(edge);
  return edge;
}

// static
Edge* State::CreateEdge(const Rule* rule, BindingEnv* env) {
  // Edges are never deleted.
  Edge* edge = new (AllocateForever(sizeof(Edge))) Edge();
  edge->rule_ = rule;
  edge->pool_ = &State::kDefaultPool;
  edge->env_ = env;
  return edge;
}

void State::AppendEdge(Edge* edge) {
  edge->id_ = edges_.size();
  edges_.push_back(edge);
}

uint64_t PathMap::Hash(StringPiece path) {
  return rapidhash(path.str_, path.len_);
}

Node* PathMap::Lookup(StringPiece path) const {
  Key key = { path, Hash(path) };
  const Shard& shard = ShardFor(key.hash);
  std::lock_guard<SpinLock> lock(shard.mutex);
  auto i = shard.map.find(key);
  return i != shard.map.end() ? i->second : NULL;
}

Node* PathMap::GetOrCreate(StringPiece path, uint64_t hash,
                           uint64_t slash_bits, bool provisional) {
  Key key = { path, hash };
  Shard& shard = ShardFor(key.hash);
  std::lock_guard<SpinLock> lock(shard.mutex);
  auto i = shard.map.find(key);
  if (i != shard.map.end())
    return i->second;
  // Nodes are never deleted.
  Node* node = new (AllocateForever(sizeof(Node))) Node(path, slash_bits);
  node->set_slash_bits_provisional(provisional);
  key.path = node->path();
  shard.map.emplace(key, node);
  return node;
}

void PathMap::Erase(StringPiece path) {
  Key key = { path, Hash(path) };
  Shard& shard = ShardFor(key.hash);
  std::lock_guard<SpinLock> lock(shard.mutex);
  shard.map.erase(key);
}

size_t PathMap::size() const {
  size_t size = 0;
  for (const Shard& shard : shards_)
    size += shard.map.size();
  return size;
}

size_t PathMap::bucket_count() const {
  size_t count = 0;
  for (const Shard& shard : shards_)
    count += shard.map.bucket_count();
  return count;
}

Node* State::GetNode(StringPiece path, uint64_t slash_bits) {
  return paths_.GetOrCreate(path, slash_bits);
}

Node* State::LookupNode(StringPiece path) const {
  return paths_.Lookup(path);
}

Node* State::SpellcheckNode(const string& path) {
  const bool kAllowReplacements = true;
  const int kMaxValidEditDistance = 3;

  int min_distance = kMaxValidEditDistance + 1;
  Node* result = NULL;
  paths_.ForEach([&](Node* node) {
    int distance = EditDistance(
        node->path(), path, kAllowReplacements, kMaxValidEditDistance);
    if (distance < min_distance) {
      min_distance = distance;
      result = node;
    }
  });
  return result;
}

namespace {
/// Fix up the slash bits of a node created out of order.
void UseSlashBits(Node* node, uint64_t slash_bits) {
  if (node->slash_bits_provisional()) {
    node->set_slash_bits(slash_bits);
    node->set_slash_bits_provisional(false);
  }
}
}  // namespace

void State::AddIn(Edge* edge, StringPiece path, uint64_t slash_bits) {
  AddIn(edge, GetNode(path, slash_bits), slash_bits);
}

void State::AddIn(Edge* edge, Node* node, uint64_t slash_bits) {
  edge->inputs_.push_back(node);
  ConnectInput(edge, node, slash_bits);
}

void State::ConnectInput(Edge* edge, Node* node, uint64_t slash_bits) {
  UseSlashBits(node, slash_bits);
  node->set_generated_by_dep_loader(false);
  node->AddOutEdge(edge);
}

bool State::AddOut(Edge* edge, StringPiece path, uint64_t slash_bits,
                   std::string* err) {
  return AddOut(edge, GetNode(path, slash_bits), slash_bits, err);
}

bool State::AddOut(Edge* edge, Node* node, uint64_t slash_bits,
                   std::string* err) {
  if (!ConnectOutput(edge, node, slash_bits, err))
    return false;
  edge->outputs_.push_back(node);
  return true;
}

bool State::ConnectOutput(Edge* edge, Node* node, uint64_t slash_bits,
                          std::string* err) {
  if (Edge* other = node->in_edge()) {
    if (other == edge) {
      *err = node->path() + " is defined as an output multiple times";
    } else {
      *err = "multiple rules generate " + node->path();
    }
    return false;
  }
  UseSlashBits(node, slash_bits);
  node->set_in_edge(edge);
  node->set_generated_by_dep_loader(false);
  return true;
}

void State::AddValidation(Edge* edge, StringPiece path, uint64_t slash_bits) {
  AddValidation(edge, GetNode(path, slash_bits), slash_bits);
}

void State::AddValidation(Edge* edge, Node* node, uint64_t slash_bits) {
  edge->validations_.push_back(node);
  ConnectValidation(edge, node, slash_bits);
}

void State::ConnectValidation(Edge* edge, Node* node, uint64_t slash_bits) {
  UseSlashBits(node, slash_bits);
  node->AddValidationOutEdge(edge);
  node->set_generated_by_dep_loader(false);
}

bool State::AddDefault(StringPiece path, string* err) {
  Node* node = LookupNode(path);
  if (!node) {
    *err = "unknown target '" + path.AsString() + "'";
    return false;
  }
  defaults_.push_back(node);
  return true;
}

vector<Node*> State::RootNodes(string* err) const {
  vector<Node*> root_nodes;
  // Search for nodes with no output.
  for (vector<Edge*>::const_iterator e = edges_.begin();
       e != edges_.end(); ++e) {
    for (vector<Node*>::const_iterator out = (*e)->outputs_.begin();
         out != (*e)->outputs_.end(); ++out) {
      if ((*out)->out_edges().empty())
        root_nodes.push_back(*out);
    }
  }

  if (!edges_.empty() && root_nodes.empty())
    *err = "could not determine root nodes of build graph";

  return root_nodes;
}

vector<Node*> State::DefaultNodes(string* err) const {
  return defaults_.empty() ? RootNodes(err) : defaults_;
}

void State::Reset() {
  paths_.ForEach([](Node* node) { node->ResetState(); });
  for (vector<Edge*>::iterator e = edges_.begin(); e != edges_.end(); ++e) {
    (*e)->outputs_ready_ = false;
    (*e)->deps_loaded_ = false;
    (*e)->mark_ = Edge::VisitNone;
    (*e)->scan_bindings_.valid = false;
    (*e)->scan_bindings_.precomputed = false;
    (*e)->scan_deps_.valid = false;
    (*e)->deps_pending_ = false;
  }
}

void State::Dump() {
  paths_.ForEach([](Node* node) {
    printf("%s %s [id:%d]\n",
           node->path().c_str(),
           node->status_known() ? (node->dirty() ? "dirty" : "clean")
                                : "unknown",
           node->id());
  });
  if (!pools_.empty()) {
    printf("resource_pools:\n");
    for (map<string, Pool*>::const_iterator it = pools_.begin();
         it != pools_.end(); ++it)
    {
      if (!it->second->name().empty()) {
        it->second->Dump();
      }
    }
  }
}
