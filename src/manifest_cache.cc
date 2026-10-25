// Copyright 2026 Google Inc. All Rights Reserved.
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

#include "manifest_cache.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include "build_log.h"
#include "disk_interface.h"
#include "eval_env.h"
#include "graph.h"
#include "hash_map.h"
#include "metrics.h"
#include "state.h"
#include "thread_pool.h"
#include "util.h"
#include "version.h"

// The cache file starts with a header that identifies the manifest and lists
// the files that make it up, with their status.  Then come arrays of
// fixed-size records (so that they can be checked and used in parallel,
// right from the file), each preceded by its 64-bit size and aligned to 8
// bytes:
// - The strings that the records refer to (as an offset and a length),
//   deduplicated.
// - The nodes: their paths and slash bits.
// - The scopes (BindingEnvs), parents first, the top-level scope first,
//   and the bindings of the scopes.
// - The rules (except the phony rule), their bindings, and the tokens of
//   their bindings.
// - The pools (except the built-in ones).
// - The edges, in order, and the nodes of the edges.
// - The bindings of the edges that the scan needs (see Edge::ScanBindings).
// - The default targets.
// Integers are in the machine's byte order.  The bindings of the scopes
// refer to the cache's contents (which are kept), rather than being copied.

namespace {

const char kMagic[] = "# ninja manifest cache\n";
const uint32_t kFormatVersion = 3;
const uint32_t kNone = 0xFFFFFFFF;
/// Indices of the built-in pools.
const uint32_t kDefaultPool = 0;
const uint32_t kConsolePool = 1;

struct StringRef {
  uint32_t offset, size;
};
struct NodeRecord {
  StringRef path;
  uint64_t slash_bits;
};
struct BindingRecord {
  StringRef key, value;
};
struct EnvRecord {
  uint32_t parent;
  /// The range of the env's bindings.
  uint32_t begin, end;
  uint32_t padding;
};
struct RuleRecord {
  uint32_t env;
  StringRef name;
  /// The range of the rule's bindings.
  uint32_t begin, end;
  uint32_t padding;
};
struct RuleBindingRecord {
  StringRef key;
  /// The range of the binding's tokens.
  uint32_t begin, end;
};
struct TokenRecord {
  uint32_t type;
  StringRef text;
  uint32_t padding;
};
struct PoolRecord {
  StringRef name;
  uint32_t depth;
  uint32_t padding;
};
struct EdgeRecord {
  uint32_t rule, pool, env;
  uint32_t outs, implicit_outs, ins, implicit, order_only, validations;
  uint32_t dyndep;
  /// The index of the edge's first node in the array of the edges' nodes.
  uint32_t nodes;
  uint32_t padding;
};

struct ScanBindingsRecord {
  enum Flags {
    kRestat = 1,
    kGenerator = 2,
    kHasDeps = 4,
  };
  uint64_t command_hash;
  StringRef depfile;
  uint32_t flags;
  uint32_t padding;
};

struct Writer {
  std::string out;
  void U32(uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }
  void U64(uint64_t v) { out.append(reinterpret_cast<const char*>(&v), 8); }
  void Str(StringPiece s) {
    U32(static_cast<uint32_t>(s.len_));
    out.append(s.str_, s.len_);
  }
  void Align() { out.resize((out.size() + 7) & ~size_t(7)); }
  void Bytes(StringPiece bytes) {
    U64(bytes.len_);
    out.append(bytes.str_, bytes.len_);
    Align();
  }
  template <typename T>
  void Array(const std::vector<T>& records) {
    U64(records.size());
    out.append(reinterpret_cast<const char*>(records.data()),
               records.size() * sizeof(T));
    Align();
  }
};

struct Reader {
  Reader(const char* begin, const char* end)
      : begin(begin), p(begin), end(end) {}
  bool Need(uint64_t size) {
    if (!ok || static_cast<uint64_t>(end - p) < size)
      ok = false;
    return ok;
  }
  uint32_t U32() {
    uint32_t v = 0;
    if (Need(4)) {
      memcpy(&v, p, 4);
      p += 4;
    }
    return v;
  }
  uint64_t U64() {
    uint64_t v = 0;
    if (Need(8)) {
      memcpy(&v, p, 8);
      p += 8;
    }
    return v;
  }
  StringPiece Str() {
    uint32_t size = U32();
    if (!Need(size))
      return StringPiece();
    StringPiece s(p, size);
    p += size;
    return s;
  }
  void Align() {
    size_t offset = ((p - begin) + 7) & ~size_t(7);
    if (Need(offset - (p - begin)))
      p = begin + offset;
  }
  StringPiece Bytes() {
    uint64_t size = U64();
    if (!Need(size))
      return StringPiece();
    StringPiece bytes(p, size);
    p += size;
    Align();
    return bytes;
  }
  /// Return the records of an array, and their number in \a count.
  template <typename T>
  const T* Array(size_t* count) {
    *count = 0;
    uint64_t size = U64();
    if (size > kNone || !Need(size * sizeof(T)))
      return nullptr;
    const T* records = reinterpret_cast<const T*>(p);
    *count = size;
    p += size * sizeof(T);
    Align();
    return records;
  }

  const char* begin;
  const char* p;
  const char* end;
  bool ok = true;
};

int64_t NowNanoseconds() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/// Check \a count items in parallel with \a check(i), which returns whether
/// the item is valid.
bool CheckAll(size_t count, const std::function<bool(size_t)>& check) {
  std::atomic<bool> ok{true};
  ParallelFor(count, 4096, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end && ok.load(std::memory_order_relaxed);
         ++i) {
      if (!check(i))
        ok = false;
    }
  });
  return ok;
}

}  // anonymous namespace

int64_t ManifestCache::recent_change_ns = 2000000000LL;

bool ManifestCache::Save(const std::string& path,
                         const std::string& input_file,
                         const ManifestParserOptions& options,
                         const State& state, std::string* err) {
  METRIC_RECORD("manifest cache save");
  Writer w;
  w.out.append(kMagic);
  w.U32(kFormatVersion);
  w.Str(kNinjaVersion);
  w.U32(options.phony_cycle_action_);
  w.Str(input_file);

  // The files.
  int64_t now = NowNanoseconds();
  w.U32(static_cast<uint32_t>(state.manifest_contents_.size()));
  for (const auto& contents : state.manifest_contents_) {
    FileStatus status;
    if (!contents->has_status || contents->path.empty() ||
        !FileStatus::Get(contents->path, &status) ||
        status != contents->status || status.mtime > now - recent_change_ns ||
        status.ctime > now - recent_change_ns)
      return false;
    w.Str(contents->path);
    w.U64(status.dev);
    w.U64(status.ino);
    w.U64(status.size);
    w.U64(static_cast<uint64_t>(status.mtime));
    w.U64(static_cast<uint64_t>(status.ctime));
  }
  w.Align();

  // The strings.
  std::string strings;
  emhash8::HashMap<StringPiece, uint32_t> string_offsets;
  bool too_big = false;
  auto string_ref = [&](StringPiece s, bool dedup) {
    if (dedup) {
      auto i = string_offsets.find(s);
      if (i != string_offsets.end())
        return StringRef{ i->second, static_cast<uint32_t>(s.len_) };
    }
    if (strings.size() + s.len_ >= kNone)
      too_big = true;
    StringRef ref = { static_cast<uint32_t>(strings.size()),
                      static_cast<uint32_t>(s.len_) };
    strings.append(s.str_, s.len_);
    if (dedup)
      string_offsets.emplace(s, ref.offset);
    return ref;
  };

  // Number the nodes, scopes, rules and pools.  All nodes of the manifest
  // are referred to by edges (and others, created concurrently for the deps
  // log, must not be included).
  emhash8::HashMap<const Node*, uint32_t> node_indices;
  std::vector<NodeRecord> nodes;
  auto node_index = [&](const Node* node) {
    auto inserted =
        node_indices.emplace(node, static_cast<uint32_t>(nodes.size()));
    if (inserted.second)
      nodes.push_back({ string_ref(node->path(), false), node->slash_bits() });
    return inserted.first->second;
  };
  emhash8::HashMap<const BindingEnv*, uint32_t> env_indices;
  std::vector<const BindingEnv*> envs;
  std::function<uint32_t(const BindingEnv*)> env_index =
      [&](const BindingEnv* env) -> uint32_t {
    auto i = env_indices.find(env);
    if (i != env_indices.end())
      return i->second;
    if (env->parent_)
      env_index(env->parent_);
    env_indices.emplace(env, static_cast<uint32_t>(envs.size()));
    envs.push_back(env);
    return static_cast<uint32_t>(envs.size() - 1);
  };
  env_index(&state.bindings_);
  std::vector<EdgeRecord> edges;
  std::vector<uint32_t> edge_nodes;
  for (const Edge* edge : state.edges_) {
    EdgeRecord record = {};
    record.env = env_index(edge->env_);
    record.outs = static_cast<uint32_t>(edge->outputs_.size());
    record.implicit_outs = edge->implicit_outs_;
    record.ins = static_cast<uint32_t>(edge->inputs_.size());
    record.implicit = edge->implicit_deps_;
    record.order_only = edge->order_only_deps_;
    record.validations = static_cast<uint32_t>(edge->validations_.size());
    record.nodes = static_cast<uint32_t>(edge_nodes.size());
    for (const Node* node : edge->outputs_)
      edge_nodes.push_back(node_index(node));
    for (const Node* node : edge->inputs_)
      edge_nodes.push_back(node_index(node));
    for (const Node* node : edge->validations_)
      edge_nodes.push_back(node_index(node));
    record.dyndep = edge->dyndep_ ? node_index(edge->dyndep_) : kNone;
    edges.push_back(record);
  }
  std::vector<uint32_t> defaults;
  for (const Node* node : state.defaults_)
    defaults.push_back(node_index(node));

  std::vector<EnvRecord> env_records;
  std::vector<BindingRecord> bindings;
  std::vector<RuleRecord> rules;
  std::vector<RuleBindingRecord> rule_bindings;
  std::vector<TokenRecord> tokens;
  emhash8::HashMap<const Rule*, uint32_t> rule_indices;
  for (const BindingEnv* env : envs) {
    EnvRecord record = {};
    record.parent = env->parent_ ? env_indices[env->parent_] : kNone;
    record.begin = static_cast<uint32_t>(bindings.size());
    for (const auto& binding : env->bindings_)
      bindings.push_back({ string_ref(binding.first, true),
                           string_ref(binding.second, true) });
    record.end = static_cast<uint32_t>(bindings.size());
    env_records.push_back(record);

    for (const auto& rule_entry : env->rules_) {
      const Rule* rule = rule_entry.second.get();
      if (rule->IsPhony())
        continue;
      rule_indices.emplace(rule, static_cast<uint32_t>(rules.size()));
      RuleRecord rule_record = {};
      rule_record.env = static_cast<uint32_t>(env_records.size() - 1);
      rule_record.name = string_ref(rule->name(), true);
      rule_record.begin = static_cast<uint32_t>(rule_bindings.size());
      for (const auto& binding : rule->bindings_) {
        RuleBindingRecord binding_record;
        binding_record.key = string_ref(binding.first, true);
        binding_record.begin = static_cast<uint32_t>(tokens.size());
        const EvalString& eval = binding.second;
        if (eval.parsed_.empty()) {
          if (!eval.single_token_.empty())
            tokens.push_back({ EvalString::RAW,
                               string_ref(eval.single_token_, true), 0 });
        } else {
          for (const auto& token : eval.parsed_)
            tokens.push_back({ static_cast<uint32_t>(token.second),
                               string_ref(token.first, true), 0 });
        }
        binding_record.end = static_cast<uint32_t>(tokens.size());
        rule_bindings.push_back(binding_record);
      }
      rule_record.end = static_cast<uint32_t>(rule_bindings.size());
      rules.push_back(rule_record);
    }
  }

  emhash8::HashMap<const Pool*, uint32_t> pool_indices;
  std::vector<PoolRecord> pools;
  for (const auto& pool : state.pools_) {
    if (pool.second == &State::kDefaultPool ||
        pool.second == &State::kConsolePool)
      continue;
    pool_indices.emplace(pool.second, static_cast<uint32_t>(pools.size() + 2));
    pools.push_back({ string_ref(pool.second->name(), true),
                      static_cast<uint32_t>(pool.second->depth()), 0 });
  }

  for (size_t i = 0; i < edges.size(); ++i) {
    const Edge* edge = state.edges_[i];
    edges[i].rule = edge->rule_->IsPhony() ? kNone : rule_indices[edge->rule_];
    if (edge->pool_ == &State::kDefaultPool)
      edges[i].pool = kDefaultPool;
    else if (edge->pool_ == &State::kConsolePool)
      edges[i].pool = kConsolePool;
    else
      edges[i].pool = pool_indices[edge->pool_];
  }
  // The bindings that the scan needs.
  std::vector<ScanBindingsRecord> scan_bindings(edges.size());
  std::vector<std::string> depfiles(edges.size());
  ParallelFor(edges.size(), 64, [&](size_t begin, size_t end) {
    std::string command;
    for (size_t i = begin; i < end; ++i) {
      Edge* edge = state.edges_[i];
      ScanBindingsRecord& record = scan_bindings[i];
      record.flags = 0;
      if (edge->GetBindingBool("restat"))
        record.flags |= ScanBindingsRecord::kRestat;
      if (edge->GetBindingBool("generator"))
        record.flags |= ScanBindingsRecord::kGenerator;
      if (!edge->GetBinding("deps").empty())
        record.flags |= ScanBindingsRecord::kHasDeps;
      else
        depfiles[i] = edge->GetUnescapedDepfile();
      command.clear();
      edge->EvaluateCommandInto(&command, /*incl_rsp_file=*/true);
      record.command_hash = BuildLog::LogEntry::HashCommand(command);
    }
  });
  for (size_t i = 0; i < edges.size(); ++i)
    scan_bindings[i].depfile = string_ref(depfiles[i], false);
  if (too_big || edge_nodes.size() >= kNone)
    return false;

  w.Bytes(strings);
  w.Array(nodes);
  w.Array(env_records);
  w.Array(bindings);
  w.Array(rules);
  w.Array(rule_bindings);
  w.Array(tokens);
  w.Array(pools);
  w.Array(edges);
  w.Array(edge_nodes);
  w.Array(scan_bindings);
  w.Array(defaults);
  w.out.append(kMagic);

  // Write the file atomically.
  std::string temp_path = path + ".tmp";
  FILE* f = fopen(temp_path.c_str(), "wb");
  if (!f) {
    *err = strerror(errno);
    return false;
  }
  bool ok = fwrite(w.out.data(), 1, w.out.size(), f) == w.out.size();
  ok = fclose(f) == 0 && ok;
  if (!ok || rename(temp_path.c_str(), path.c_str()) != 0) {
    *err = strerror(errno);
    platformAwareUnlink(temp_path.c_str());
    return false;
  }
  return true;
}

bool ManifestCache::Load(const std::string& path,
                         const std::string& input_file,
                         const ManifestParserOptions& options, State* state) {
  METRIC_RECORD("manifest cache load");
  if (!state->edges_.empty())
    return false;
  std::unique_ptr<FileContents> contents(new FileContents);
  RealDiskInterface disk;
  std::string err;
  if (disk.LoadFile(path, 0, contents.get(), &err) != FileReader::Okay)
    return false;
  // The records are used in place.
  if (reinterpret_cast<uintptr_t>(contents->data) % 8 != 0)
    return false;
  Reader r(contents->data, contents->data + contents->size);
  const size_t kMagicSize = sizeof(kMagic) - 1;
  if (!r.Need(kMagicSize) || memcmp(r.p, kMagic, kMagicSize) != 0)
    return false;
  r.p += kMagicSize;
  if (r.U32() != kFormatVersion || r.Str() != kNinjaVersion ||
      r.U32() != static_cast<uint32_t>(options.phony_cycle_action_) ||
      r.Str() != input_file || !r.ok)
    return false;

  // Check that the files didn't change.
  uint32_t file_count = r.U32();
  std::vector<std::pair<std::string, FileStatus> > files;
  for (uint32_t i = 0; r.ok && i < file_count; ++i) {
    std::string file = r.Str().AsString();
    FileStatus status;
    status.dev = r.U64();
    status.ino = r.U64();
    status.size = r.U64();
    status.mtime = static_cast<int64_t>(r.U64());
    status.ctime = static_cast<int64_t>(r.U64());
    files.emplace_back(std::move(file), status);
  }
  r.Align();
  if (!r.ok || files.empty())
    return false;
  if (!CheckAll(files.size(), [&](size_t i) {
        FileStatus status;
        return FileStatus::Get(files[i].first, &status) &&
               status == files[i].second;
      }))
    return false;

  StringPiece strings = r.Bytes();
  size_t node_count, env_count, binding_count, rule_count,
      rule_binding_count, token_count, pool_count, edge_count,
      edge_node_count, default_count;
  const NodeRecord* nodes = r.Array<NodeRecord>(&node_count);
  const EnvRecord* env_records = r.Array<EnvRecord>(&env_count);
  const BindingRecord* bindings = r.Array<BindingRecord>(&binding_count);
  const RuleRecord* rules = r.Array<RuleRecord>(&rule_count);
  const RuleBindingRecord* rule_bindings =
      r.Array<RuleBindingRecord>(&rule_binding_count);
  const TokenRecord* tokens = r.Array<TokenRecord>(&token_count);
  const PoolRecord* pools = r.Array<PoolRecord>(&pool_count);
  const EdgeRecord* edges = r.Array<EdgeRecord>(&edge_count);
  const uint32_t* edge_nodes = r.Array<uint32_t>(&edge_node_count);
  size_t scan_bindings_count;
  const ScanBindingsRecord* scan_bindings =
      r.Array<ScanBindingsRecord>(&scan_bindings_count);
  const uint32_t* defaults = r.Array<uint32_t>(&default_count);
  if (!r.ok || !r.Need(kMagicSize) || memcmp(r.p, kMagic, kMagicSize) != 0)
    return false;

  // Check everything before changing the State.
  auto string_ok = [&](StringRef ref) {
    return ref.offset <= strings.len_ && ref.size <= strings.len_ - ref.offset;
  };
  auto str = [&](StringRef ref) {
    return StringPiece(strings.str_ + ref.offset, ref.size);
  };
  if (env_count == 0 || env_records[0].parent != kNone ||
      scan_bindings_count != edge_count)
    return false;
  for (size_t i = 0; i < rule_count; ++i) {
    const RuleRecord& rule = rules[i];
    if (rule.env >= env_count || !string_ok(rule.name) ||
        rule.begin > rule.end || rule.end > rule_binding_count)
      return false;
  }
  for (size_t i = 0; i < rule_binding_count; ++i) {
    const RuleBindingRecord& binding = rule_bindings[i];
    if (!string_ok(binding.key) || binding.begin > binding.end ||
        binding.end > token_count)
      return false;
  }
  for (size_t i = 0; i < token_count; ++i) {
    if (tokens[i].type > EvalString::SPECIAL || !string_ok(tokens[i].text))
      return false;
  }
  for (size_t i = 0; i < pool_count; ++i) {
    if (!string_ok(pools[i].name) || pools[i].name.size == 0 ||
        pools[i].depth > INT32_MAX ||
        state->LookupPool(str(pools[i].name).AsString()))
      return false;
  }
  if (!CheckAll(node_count, [&](size_t i) {
        return string_ok(nodes[i].path) && nodes[i].path.size != 0;
      }) ||
      !CheckAll(env_count, [&](size_t i) {
        const EnvRecord& env = env_records[i];
        return (i == 0 || env.parent < i) && env.begin <= env.end &&
               env.end <= binding_count;
      }) ||
      !CheckAll(binding_count, [&](size_t i) {
        return string_ok(bindings[i].key) && string_ok(bindings[i].value);
      }) ||
      !CheckAll(edge_count, [&](size_t i) {
        const EdgeRecord& edge = edges[i];
        uint64_t count = static_cast<uint64_t>(edge.outs) + edge.ins +
                         edge.validations;
        return (edge.rule < rule_count || edge.rule == kNone) &&
               edge.pool < pool_count + 2 && edge.env < env_count &&
               edge.outs != 0 && edge.implicit_outs <= edge.outs &&
               static_cast<uint64_t>(edge.implicit) + edge.order_only <=
                   edge.ins &&
               (edge.dyndep < node_count || edge.dyndep == kNone) &&
               edge.nodes <= edge_node_count &&
               count <= edge_node_count - edge.nodes &&
               string_ok(scan_bindings[i].depfile);
      }) ||
      !CheckAll(edge_node_count,
                [&](size_t i) { return edge_nodes[i] < node_count; }) ||
      !CheckAll(default_count,
                [&](size_t i) { return defaults[i] < node_count; }))
    return false;

  // Build the State.  The pools.
  std::vector<Pool*> pool_ptrs = { &State::kDefaultPool, &State::kConsolePool };
  for (size_t i = 0; i < pool_count; ++i) {
    pool_ptrs.push_back(new Pool(str(pools[i].name).AsString(),
                                 static_cast<int>(pools[i].depth)));
    state->AddPool(pool_ptrs.back());
  }

  // The scopes, with their rules.  The bindings refer to the cache.
  std::vector<BindingEnv*> envs(env_count);
  envs[0] = &state->bindings_;
  ParallelFor(env_count, 1024, [&](size_t begin, size_t end) {
    for (size_t i = std::max<size_t>(begin, 1); i < end; ++i)
      envs[i] = new BindingEnv;
  });
  ParallelFor(env_count, 1024, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      const EnvRecord& record = env_records[i];
      if (i != 0)
        envs[i]->parent_ = envs[record.parent];
      for (uint32_t j = record.begin; j < record.end; ++j)
        envs[i]->AddBindingView(str(bindings[j].key), str(bindings[j].value));
    }
  });
  std::vector<const Rule*> rule_ptrs;
  for (size_t i = 0; i < rule_count; ++i) {
    const RuleRecord& record = rules[i];
    std::unique_ptr<Rule> rule(new Rule(str(record.name).AsString()));
    for (uint32_t j = record.begin; j < record.end; ++j) {
      EvalString eval;
      for (uint32_t k = rule_bindings[j].begin; k < rule_bindings[j].end;
           ++k) {
        if (tokens[k].type == EvalString::RAW)
          eval.AddText(str(tokens[k].text));
        else
          eval.AddSpecial(str(tokens[k].text));
      }
      rule->AddBinding(str(rule_bindings[j].key).AsString(), eval);
    }
    rule_ptrs.push_back(rule.get());
    envs[record.env]->AddRule(std::move(rule));
  }

  // The nodes, which may already exist if the deps log created them.
  std::vector<Node*> node_ptrs(node_count);
  ParallelFor(node_count, 1024, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i)
      node_ptrs[i] =
          state->paths_.GetOrCreate(str(nodes[i].path), nodes[i].slash_bits);
  });

  // The edges.
  std::vector<Edge*> edge_ptrs(edge_count);
  const Rule* phony = state->bindings_.LookupRuleCurrentScope("phony");
  ParallelFor(edge_count, 256, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      const EdgeRecord& record = edges[i];
      Edge* edge = State::CreateEdge(
          record.rule == kNone ? phony : rule_ptrs[record.rule],
          envs[record.env]);
      edge->pool_ = pool_ptrs[record.pool];
      const uint32_t* index = edge_nodes + record.nodes;
      edge->outputs_.resize(record.outs);
      for (Node*& node : edge->outputs_)
        node = node_ptrs[*index++];
      edge->implicit_outs_ = record.implicit_outs;
      edge->inputs_.resize(record.ins);
      for (Node*& node : edge->inputs_)
        node = node_ptrs[*index++];
      edge->implicit_deps_ = record.implicit;
      edge->order_only_deps_ = record.order_only;
      edge->validations_.resize(record.validations);
      for (Node*& node : edge->validations_)
        node = node_ptrs[*index++];
      if (record.dyndep != kNone)
        edge->dyndep_ = node_ptrs[record.dyndep];
      const ScanBindingsRecord& scan = scan_bindings[i];
      Edge::ScanBindings& b = edge->scan_bindings_;
      b.precomputed = true;
      b.restat = scan.flags & ScanBindingsRecord::kRestat;
      b.generator = scan.flags & ScanBindingsRecord::kGenerator;
      b.has_deps = scan.flags & ScanBindingsRecord::kHasDeps;
      b.depfile = str(scan.depfile).AsString();
      b.command_hash = scan.command_hash;
      b.command_hash_valid = true;
      edge_ptrs[i] = edge;
    }
  });
  for (Edge* edge : edge_ptrs)
    state->AppendEdge(edge);

  // Connect the nodes to the edges, in the order of the edges.  Split the
  // work by node, so that each node's edges are added in order.
  size_t buckets = TaskGroup::Concurrency();
  ParallelFor(buckets, 1, [&](size_t begin, size_t end) {
    for (size_t bucket = begin; bucket < end; ++bucket) {
      for (size_t i = 0; i < edge_count; ++i) {
        Edge* edge = edge_ptrs[i];
        const EdgeRecord& record = edges[i];
        const uint32_t* indices = edge_nodes + record.nodes;
        size_t count = record.outs + record.ins + record.validations;
        for (size_t n = 0; n < count; ++n) {
          uint32_t index = indices[n];
          if (index % buckets != bucket)
            continue;
          Node* node = node_ptrs[index];
          if (n < record.outs)
            node->set_in_edge(edge);
          else if (n < record.outs + record.ins)
            node->AddOutEdge(edge);
          else
            node->AddValidationOutEdge(edge);
          node->set_generated_by_dep_loader(false);
          // Nodes that the deps log created have provisional slash bits.
          if (node->slash_bits_provisional()) {
            node->set_slash_bits(nodes[index].slash_bits);
            node->set_slash_bits_provisional(false);
          }
        }
        if (record.dyndep != kNone && record.dyndep % buckets == bucket)
          edge->dyndep_->set_dyndep_pending(true);
      }
    }
  });

  for (size_t i = 0; i < default_count; ++i)
    state->defaults_.push_back(node_ptrs[defaults[i]]);
  state->manifest_contents_.push_back(std::move(contents));
  return true;
}
