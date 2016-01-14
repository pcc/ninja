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

#ifndef NINJA_EVAL_ENV_H_
#define NINJA_EVAL_ENV_H_

#include <map>
#include <string>
#include <vector>
using namespace std;

#include "mblock.h"
#include "string_piece.h"

struct Edge;
struct Node;
struct Rule;

/// An interface for a scope for variable (e.g. "$foo") lookups.
struct Env {
  enum Kind { kEdgeEnv, kBindingEnv } kind_;
  Env(Kind kind) : kind_(kind) {}
  string LookupVariable(const string& var);
};

/// A tokenized string that contains variable references.
/// Can be evaluated relative to an Env.
struct EvalString {
  string Evaluate(Env* env) const;

  void Clear() { parsed_.clear(); }
  bool empty() const { return parsed_.empty(); }

  void AddText(StringPiece text);
  void AddSpecial(StringPiece text);

  /// Construct a human-readable representation of the parsed state,
  /// for use in tests.
  string Serialize() const;

private:
  enum TokenType { RAW, SPECIAL };
  typedef mblock_vector<pair<mblock_string, TokenType> >::type TokenList;
  TokenList parsed_;
};

/// An invokable build command and associated metadata (description, etc.).
struct Rule {
  explicit Rule(const string& name) : name_(name.c_str()) {}

  StringPiece name() const { return name_; }

  void AddBinding(const string& key, const EvalString& val);

  static bool IsReservedBinding(const string& var);

  const EvalString* GetBinding(const string& key) const;

 private:
  // Allow the parsers to reach into this object and fill out its fields.
  friend struct ManifestParser;

  mblock_string name_;
  typedef mblock_map<mblock_string, EvalString>::type Bindings;
  Bindings bindings_;
};

/// An Env which contains a mapping of variables to values
/// as well as a pointer to a parent scope.
struct BindingEnv : public Env {
  BindingEnv() : Env(kBindingEnv), parent_(NULL) {}
  explicit BindingEnv(BindingEnv* parent) : Env(kBindingEnv), parent_(parent) {}

  string LookupVariable(const string& var);

  void AddRule(const Rule* rule);
  const Rule* LookupRule(const string& rule_name);
  const Rule* LookupRuleCurrentScope(const string& rule_name);
  const mblock_map<mblock_string, const Rule*>::type& GetRules() const;

  void AddBinding(const string& key, const string& val);

  /// This is tricky.  Edges want lookup scope to go in this order:
  /// 1) value set on edge itself (edge_->env_)
  /// 2) value set on rule, with expansion in the edge's scope
  /// 3) value set on enclosing scope of edge (edge_->env_->parent_)
  /// This function takes as parameters the necessary info to do (2).
  string LookupWithFallback(const string& var, const EvalString* eval,
                            Env* env);

private:
  mblock_map<mblock_string, mblock_string>::type bindings_;
  mblock_map<mblock_string, const Rule*>::type rules_;
  BindingEnv* parent_;
};

/// An Env for an Edge, providing $in and $out.
struct EdgeEnv : public Env {
  enum EscapeKind { kShellEscape, kDoNotEscape };

  EdgeEnv(Edge* edge, EscapeKind escape)
      : Env(kEdgeEnv), edge_(edge), escape_in_out_(escape), recursive_(false) {}
  string LookupVariable(const string& var);

  /// Given a span of Nodes, construct a list of paths suitable for a command
  /// line.
  string MakePathList(Node** begin, Node** end, char sep);

 private:
  vector<string> lookups_;
  Edge* edge_;
  EscapeKind escape_in_out_;
  bool recursive_;
};

inline string Env::LookupVariable(const string& var) {
  if (kind_ == kBindingEnv)
    return static_cast<BindingEnv *>(this)->LookupVariable(var);
  else
    return static_cast<EdgeEnv *>(this)->LookupVariable(var);
}

#endif  // NINJA_EVAL_ENV_H_
