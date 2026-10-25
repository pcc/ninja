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
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "hash_map.h"
#include "string_piece.h"
#include "string_piece_util.h"

struct Rule;

/// A signature of a variable name, for quickly checking that a set of
/// names doesn't contain it: a set has a mask of the signatures of its
/// names, which includes the signature of every name in the set.
inline uint64_t VariableNameSignature(StringPiece name) {
  if (name.len_ == 0)
    return 1;
  unsigned bits = static_cast<unsigned>(name.len_) * 31 +
                  static_cast<unsigned char>(name.str_[0]) * 7 +
                  static_cast<unsigned char>(name.str_[name.len_ - 1]);
  return uint64_t(1) << (bits & 63);
}

/// An interface for a scope for variable (e.g. "$foo") lookups.
struct Env {
  virtual ~Env() {}
  virtual std::string LookupVariable(StringPiece var) = 0;

  /// Append the value of \a var to \a out.  Equivalent to appending the
  /// result of LookupVariable(), but implementations can avoid creating
  /// temporary strings.
  virtual void AppendVariable(StringPiece var, std::string* out) {
    out->append(LookupVariable(var));
  }
};

/// A tokenized string that contains variable references.
/// Can be evaluated relative to an Env.
struct EvalString {
  /// @return The evaluated string with variable expanded using value found in
  ///         environment @a env.
  std::string Evaluate(Env* env) const;

  /// Like Evaluate(), but appends the result to \a out.
  void EvaluateInto(Env* env, std::string* out) const;

  /// @return The string with variables not expanded.
  std::string Unparse() const;

  void Clear() { parsed_.clear(); single_token_.clear(); }
  bool empty() const { return parsed_.empty() && single_token_.empty(); }

  void AddText(StringPiece text);
  void AddSpecial(StringPiece text);

  /// Construct a human-readable representation of the parsed state,
  /// for use in tests.
  std::string Serialize() const;

private:
  friend struct ManifestCache;
  enum TokenType { RAW, SPECIAL };
  typedef std::vector<std::pair<std::string, TokenType> > TokenList;
  TokenList parsed_;

  // If we hold only a single RAW token, then we keep it here instead of
  // pushing it on TokenList. This saves a bunch of allocations for
  // what is a common case. If parsed_ is nonempty, then this value
  // must be ignored.
  std::string single_token_;
};

/// An invocable build command and associated metadata (description, etc.).
struct Rule {
  explicit Rule(const std::string& name) : name_(name) {}

  static std::unique_ptr<Rule> Phony();

  bool IsPhony() const;

  const std::string& name() const { return name_; }

  void AddBinding(const std::string& key, const EvalString& val);

  static bool IsReservedBinding(StringPiece var);

  const EvalString* GetBinding(StringPiece key) const;

 private:
  // Allow the parsers to reach into this object and fill out its fields.
  friend struct ManifestParser;
  friend struct ManifestCache;

  std::string name_;
  typedef std::map<std::string, EvalString, StringPieceLess> Bindings;
  Bindings bindings_;
  /// See VariableNameSignature().
  uint64_t binding_mask_ = 0;
  bool phony_ = false;
};

/// An Env which contains a mapping of variables to values
/// as well as a pointer to a parent scope.
struct BindingEnv : public Env {
  BindingEnv() : parent_(NULL) {}
  explicit BindingEnv(BindingEnv* parent) : parent_(parent) {}

  virtual ~BindingEnv() {}
  std::string LookupVariable(StringPiece var) override;
  void AppendVariable(StringPiece var, std::string* out) override;

  void AddRule(std::unique_ptr<const Rule> rule);
  const Rule* LookupRule(StringPiece rule_name);
  const Rule* LookupRuleCurrentScope(StringPiece rule_name);
  const std::map<std::string, std::unique_ptr<const Rule>, StringPieceLess>&
  GetRules() const;

  void AddBinding(const std::string& key, StringPiece val);

  /// Like AddBinding(), but without copying \a key and \a val, which must
  /// stay valid as long as the BindingEnv.
  void AddBindingView(StringPiece key, StringPiece val);

  /// Keep \a value as long as the BindingEnv, for AddBindingView().
  StringPiece Keep(std::string value);

  /// This is tricky.  Edges want lookup scope to go in this order:
  /// 1) value set on edge itself (edge_->env_)
  /// 2) value set on rule, with expansion in the edge's scope
  /// 3) value set on enclosing scope of edge (edge_->env_->parent_)
  /// This function takes as parameters the necessary info to do (2).
  std::string LookupWithFallback(StringPiece var, const EvalString* eval,
                                 Env* env);
  /// Like LookupWithFallback(), but appends the result to \a out.
  void AppendWithFallback(StringPiece var, const EvalString* eval, Env* env,
                          std::string* out);

private:
  friend struct ManifestCache;

  /// Return the value of the binding for \a var in this scope, or null.
  const StringPiece* Find(StringPiece var) const;

  /// The bindings: few, usually, so they are in a vector.
  std::vector<std::pair<StringPiece, StringPiece> > bindings_;
  /// See VariableNameSignature().
  uint64_t binding_mask_ = 0;
  /// An index of bindings_, once there are many.
  std::unique_ptr<std::unordered_map<StringPiece, size_t> > index_;
  /// Storage for the keys and values that are not views.
  std::vector<std::unique_ptr<std::string> > storage_;

  std::map<std::string, std::unique_ptr<const Rule>, StringPieceLess> rules_;
  /// An index of rules_, for faster lookups.
  std::unordered_map<StringPiece, const Rule*> rule_index_;
  BindingEnv* parent_;
};

#endif  // NINJA_EVAL_ENV_H_
