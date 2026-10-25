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

// The cache needs the status of files (see FileStatus), which is only
// implemented on POSIX systems.
#ifndef _WIN32

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>

#include "build_log.h"
#include "disk_interface.h"
#include "graph.h"
#include "state.h"
#include "test.h"
#include "util.h"

using namespace std;

namespace {

const char kCachePath[] = ".ninja_manifest_cache";

/// Describe everything about \a state that the manifest determines.
string Describe(State* state) {
  string out;
  map<BindingEnv*, size_t> env_indices;
  vector<BindingEnv*> envs;
  auto describe_env = [&](BindingEnv* env) {
    auto inserted = env_indices.emplace(env, envs.size());
    if (inserted.second)
      envs.push_back(env);
    out += " env" + to_string(inserted.first->second);
  };
  auto describe_node = [&](const Node* node) {
    out += " " + node->path() + "/" + to_string(node->slash_bits());
  };
  for (Edge* edge : state->edges_) {
    out += "edge " + edge->rule().name() + " pool " + edge->pool()->name() +
           " " + to_string(edge->pool()->depth());
    describe_env(edge->env_);
    out += "\n outs";
    for (Node* node : edge->outputs_)
      describe_node(node);
    out += " implicit " + to_string(edge->implicit_outs_) + "\n ins";
    for (Node* node : edge->inputs_)
      describe_node(node);
    out += " implicit " + to_string(edge->implicit_deps_) + " order-only " +
           to_string(edge->order_only_deps_) + "\n validations";
    for (Node* node : edge->validations_)
      describe_node(node);
    if (edge->dyndep_)
      out += "\n dyndep " + edge->dyndep_->path();
    // The bindings that matter, as evaluated in the edge's scope.
    for (const char* var : { "command", "description", "depfile", "deps",
                             "dyndep", "pool", "restat", "generator", "rspfile",
                             "rspfile_content", "msvc_deps_prefix", "x", "y" })
      out += string("\n ") + var + "=" + edge->GetBinding(var);
    out += "\n";
  }
  vector<Node*> nodes;
  state->paths_.ForEach([&](Node* node) { nodes.push_back(node); });
  sort(nodes.begin(), nodes.end(),
       [](Node* a, Node* b) { return a->path() < b->path(); });
  for (Node* node : nodes) {
    out += "node";
    describe_node(node);
    out += " in " + (node->in_edge() ? to_string(node->in_edge()->id_)
                                     : string("-"));
    out += " out";
    for (Edge* edge : node->out_edges())
      out += " " + to_string(edge->id_);
    out += " validation-out";
    for (Edge* edge : node->validation_out_edges())
      out += " " + to_string(edge->id_);
    out += " dep-loader " + to_string(node->generated_by_dep_loader()) +
           " dyndep-pending " + to_string(node->dyndep_pending()) +
           " provisional " + to_string(node->slash_bits_provisional()) + "\n";
  }
  for (Node* node : state->defaults_)
    out += "default " + node->path() + "\n";
  for (const auto& pool : state->pools_)
    out += "pool " + pool.first + " " + to_string(pool.second->depth()) + "\n";
  for (size_t i = 0; i < envs.size(); ++i) {
    out += "env" + to_string(i) + " rules";
    for (const auto& rule : envs[i]->GetRules()) {
      out += " " + rule.first;
      for (const char* var : { "command", "description", "depfile", "pool" }) {
        if (const EvalString* eval = rule.second->GetBinding(var))
          out += string(" ") + var + "=" + eval->Serialize();
      }
    }
    out += "\n";
  }
  for (const char* var : { "builddir", "a", "b", "x", "y" })
    out += string(var) + "=" + state->bindings_.LookupVariable(var) + "\n";
  return out;
}

/// Check that the bindings precomputed for the scan are right.
void CheckScanBindings(State* state) {
  for (Edge* edge : state->edges_) {
    const Edge::ScanBindings& b = edge->scan_bindings_;
    ASSERT_TRUE(b.precomputed);
    EXPECT_EQ(edge->GetBindingBool("restat"), b.restat);
    EXPECT_EQ(edge->GetBindingBool("generator"), b.generator);
    bool has_deps = !edge->GetBinding("deps").empty();
    EXPECT_EQ(has_deps, b.has_deps);
    EXPECT_EQ(has_deps ? "" : edge->GetUnescapedDepfile(), b.depfile);
    EXPECT_TRUE(b.command_hash_valid);
    EXPECT_EQ(BuildLog::LogEntry::HashCommand(edge->EvaluateCommand(true)),
              b.command_hash);
  }
}

struct ManifestCacheTest : public testing::Test {
  void SetUp() override {
    temp_dir_.CreateAndEnter("ManifestCacheTest");
    saved_recent_change_ns_ = ManifestCache::recent_change_ns;
    // The files are written right before saving.
    ManifestCache::recent_change_ns = -1000000000000LL;
  }
  void TearDown() override {
    ManifestCache::recent_change_ns = saved_recent_change_ns_;
    temp_dir_.Cleanup();
  }

  /// Parse "build.ninja", save the cache, load it into a new State and
  /// check that it's the same.
  void CheckRoundTrip(const string& input = "build.ninja") {
    State parsed;
    RealDiskInterface disk;
    ManifestParser parser(&parsed, &disk);
    string err;
    ASSERT_TRUE(parser.Load(input, &err)) << err;
    ASSERT_TRUE(ManifestCache::Save(kCachePath, input, ManifestParserOptions(),
                                    parsed, &err))
        << err;

    State loaded;
    ASSERT_TRUE(ManifestCache::Load(kCachePath, input, ManifestParserOptions(),
                                    &loaded));
    EXPECT_EQ(Describe(&parsed), Describe(&loaded));
    CheckScanBindings(&loaded);
  }

  bool WriteFile(const string& path, const string& contents) {
    return disk_.WriteFile(path, contents, false);
  }

  RealDiskInterface disk_;
  ScopedTempDir temp_dir_;
  int64_t saved_recent_change_ns_;
};

TEST_F(ManifestCacheTest, RoundTrip) {
  ASSERT_TRUE(WriteFile("build.ninja",
"ninja_required_version = 1.5\n"
"a = top\n"
"b = $a-b\n"
"pool link\n"
"  depth = 3\n"
"pool unused\n"
"  depth = 1\n"
"rule cc\n"
"  command = cc $in -o $out $x\n"
"  description = CC $out\n"
"  depfile = $out.d\n"
"  deps = gcc\n"
"rule link\n"
"  command = link $in -o $out\n"
"  pool = link\n"
"  restat = 1\n"
"  rspfile = $out.rsp\n"
"  rspfile_content = $in_newline\n"
"rule regen\n"
"  command = regen\n"
"  generator = 1\n"
"  depfile = build.ninja.d\n"
"build build.ninja: regen gen.py\n"
"build a.o: cc a.c | a.h || gen\n"
"  x = -O2\n"
"build b.o: cc b.c\n"
"build out | out.map: link a.o b.o |@ check\n"
"build check: phony\n"
"build gen: phony\n"
"build dd: phony\n"
"build dyn: cc dyn.in dd\n"
"  dyndep = dd\n"
"  pool = console\n"
"include inc.ninja\n"
"subninja sub/build.ninja\n"
"default out\n"
"default a.o check\n"));
  ASSERT_TRUE(WriteFile("inc.ninja",
"y = from-include\n"
"build inc.o: cc inc.c\n"
"  y = $y-edge\n"));
  ASSERT_EQ(0, mkdir("sub", 0777));
  ASSERT_TRUE(WriteFile("sub/build.ninja",
"a = sub\n"
"rule cc\n"
"  command = subcc $in $out $b\n"
"build sub.o: cc sub.c a.h\n"
"build sub2.o: link sub.o\n"
"  x = $a\n"));
  CheckRoundTrip();
}

TEST_F(ManifestCacheTest, Empty) {
  ASSERT_TRUE(WriteFile("build.ninja", "builddir = out\n"));
  CheckRoundTrip();
}

/// Check the manifest given by $NINJA_MANIFEST_CACHE_TEST_FILE, if set (it
/// is opened relative to $NINJA_MANIFEST_CACHE_TEST_DIR).
TEST_F(ManifestCacheTest, External) {
  const char* dir = getenv("NINJA_MANIFEST_CACHE_TEST_DIR");
  const char* file = getenv("NINJA_MANIFEST_CACHE_TEST_FILE");
  if (!dir || !file)
    return;
  string cache = temp_dir_.start_dir_ + "/" + temp_dir_.temp_dir_name_ + "/" +
                 kCachePath;
  ASSERT_EQ(0, chdir(dir));
  State parsed;
  RealDiskInterface disk;
  ManifestParser parser(&parsed, &disk);
  string err;
  ASSERT_TRUE(parser.Load(file, &err)) << err;
  ASSERT_TRUE(
      ManifestCache::Save(cache, file, ManifestParserOptions(), parsed, &err))
      << err;
  State loaded;
  ASSERT_TRUE(
      ManifestCache::Load(cache, file, ManifestParserOptions(), &loaded));
  EXPECT_TRUE(Describe(&parsed) == Describe(&loaded));
  CheckScanBindings(&loaded);
  ASSERT_EQ(0, chdir((temp_dir_.start_dir_ + "/" + temp_dir_.temp_dir_name_)
                         .c_str()));
}

TEST_F(ManifestCacheTest, NodesFromDepsLog) {
  ASSERT_TRUE(WriteFile("build.ninja",
"rule cc\n"
"  command = cc $in\n"
"build a.o: cc a.c\n"));
  State parsed;
  RealDiskInterface disk;
  ManifestParser parser(&parsed, &disk);
  string err;
  ASSERT_TRUE(parser.Load("build.ninja", &err)) << err;
  ASSERT_TRUE(ManifestCache::Save(kCachePath, "build.ninja",
                                  ManifestParserOptions(), parsed, &err));

  // Nodes may exist before the manifest is loaded.
  State loaded;
  Node* a_c = loaded.paths_.GetOrCreate("a.c", 1, /*provisional=*/true);
  Node* h = loaded.paths_.GetOrCreate("a.h", 1, /*provisional=*/true);
  ASSERT_TRUE(ManifestCache::Load(kCachePath, "build.ninja",
                                  ManifestParserOptions(), &loaded));
  EXPECT_EQ(a_c, loaded.LookupNode("a.c"));
  EXPECT_EQ(0u, a_c->slash_bits());
  EXPECT_FALSE(a_c->slash_bits_provisional());
  EXPECT_FALSE(a_c->generated_by_dep_loader());
  EXPECT_TRUE(h->slash_bits_provisional());
  EXPECT_TRUE(h->generated_by_dep_loader());
  EXPECT_EQ(1u, loaded.edges_.size());
}

TEST_F(ManifestCacheTest, Invalidation) {
  ASSERT_TRUE(WriteFile("build.ninja",
"rule cc\n"
"  command = cc $in\n"
"include inc.ninja\n"));
  ASSERT_TRUE(WriteFile("inc.ninja", "build a.o: cc a.c\n"));
  State parsed;
  RealDiskInterface disk;
  ManifestParser parser(&parsed, &disk);
  string err;
  ASSERT_TRUE(parser.Load("build.ninja", &err)) << err;
  ASSERT_TRUE(ManifestCache::Save(kCachePath, "build.ninja",
                                  ManifestParserOptions(), parsed, &err));

  {
    // Other options or another manifest don't use the cache.
    State state;
    ManifestParserOptions options;
    options.phony_cycle_action_ = kPhonyCycleActionError;
    EXPECT_FALSE(
        ManifestCache::Load(kCachePath, "build.ninja", options, &state));
    EXPECT_FALSE(ManifestCache::Load(kCachePath, "./build.ninja",
                                     ManifestParserOptions(), &state));
    EXPECT_TRUE(state.edges_.empty());
  }

  // Changing an included file invalidates the cache.  (Its size changes
  // too: its mtime may not, so soon, with coarse timestamps.)
  ASSERT_TRUE(WriteFile("inc.ninja", "build bb.o: cc b.c\n"));
  State state;
  EXPECT_FALSE(ManifestCache::Load(kCachePath, "build.ninja",
                                   ManifestParserOptions(), &state));
  EXPECT_TRUE(state.edges_.empty());
  EXPECT_TRUE(state.LookupNode("a.o") == NULL);
}

TEST_F(ManifestCacheTest, RecentChange) {
  ManifestCache::recent_change_ns = 1000000000000LL;
  ASSERT_TRUE(WriteFile("build.ninja", "build a: phony\n"));
  State parsed;
  RealDiskInterface disk;
  ManifestParser parser(&parsed, &disk);
  string err;
  ASSERT_TRUE(parser.Load("build.ninja", &err)) << err;
  EXPECT_FALSE(ManifestCache::Save(kCachePath, "build.ninja",
                                   ManifestParserOptions(), parsed, &err));
  EXPECT_EQ(-1, access(kCachePath, F_OK));
}

TEST_F(ManifestCacheTest, Corrupt) {
  ASSERT_TRUE(WriteFile("build.ninja",
"rule cc\n"
"  command = cc $in\n"
"build a.o: cc a.c\n"
"default a.o\n"));
  State parsed;
  RealDiskInterface disk;
  ManifestParser parser(&parsed, &disk);
  string err;
  ASSERT_TRUE(parser.Load("build.ninja", &err)) << err;
  ASSERT_TRUE(ManifestCache::Save(kCachePath, "build.ninja",
                                  ManifestParserOptions(), parsed, &err));
  string contents;
  ASSERT_EQ(FileReader::Okay,
            disk.ReadFile(kCachePath, &contents, &err));

  // Truncated caches are not used.
  for (size_t size = 0; size < contents.size(); ++size) {
    ASSERT_TRUE(WriteFile(kCachePath, contents.substr(0, size)));
    State state;
    EXPECT_FALSE(ManifestCache::Load(kCachePath, "build.ninja",
                                     ManifestParserOptions(), &state));
  }
}

}  // anonymous namespace

#endif  // _WIN32
