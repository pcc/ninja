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

#include <assert.h>

#include "eval_env.h"

using namespace std;

const StringPiece* BindingEnv::Find(StringPiece var) const {
  if (!(binding_mask_ & VariableNameSignature(var)))
    return NULL;
  if (index_) {
    auto i = index_->find(var);
    return i != index_->end() ? &bindings_[i->second].second : NULL;
  }
  for (const auto& binding : bindings_) {
    if (binding.first == var)
      return &binding.second;
  }
  return NULL;
}

std::string BindingEnv::LookupVariable(StringPiece var) {
  for (BindingEnv* env = this; env; env = env->parent_) {
    if (const StringPiece* value = env->Find(var))
      return value->AsString();
  }
  return "";
}

void BindingEnv::AppendVariable(StringPiece var, std::string* out) {
  for (BindingEnv* env = this; env; env = env->parent_) {
    if (const StringPiece* value = env->Find(var)) {
      out->append(value->str_, value->len_);
      return;
    }
  }
}

void BindingEnv::AddBinding(const std::string& key, StringPiece val) {
  storage_.emplace_back(new std::string(key));
  StringPiece key_view = *storage_.back();
  storage_.emplace_back(new std::string(val.str_, val.len_));
  AddBindingView(key_view, *storage_.back());
}

StringPiece BindingEnv::Keep(std::string value) {
  storage_.emplace_back(new std::string(std::move(value)));
  return *storage_.back();
}

void BindingEnv::AddBindingView(StringPiece key, StringPiece val) {
  if (StringPiece* value = const_cast<StringPiece*>(Find(key))) {
    *value = val;
    return;
  }
  bindings_.emplace_back(key, val);
  binding_mask_ |= VariableNameSignature(key);
  const size_t kMaxUnindexed = 16;
  if (index_) {
    index_->emplace(key, bindings_.size() - 1);
  } else if (bindings_.size() > kMaxUnindexed) {
    index_.reset(new std::unordered_map<StringPiece, size_t>);
    for (size_t i = 0; i < bindings_.size(); ++i)
      index_->emplace(bindings_[i].first, i);
  }
}

void BindingEnv::AddRule(std::unique_ptr<const Rule> rule) {
  assert(LookupRuleCurrentScope(rule->name()) == NULL);
  const Rule* r = rule.get();
  rules_[rule->name()] = std::move(rule);
  rule_index_[r->name()] = r;
}

const Rule* BindingEnv::LookupRuleCurrentScope(StringPiece rule_name) {
  auto i = rule_index_.find(rule_name);
  if (i == rule_index_.end())
    return NULL;
  return i->second;
}

const Rule* BindingEnv::LookupRule(StringPiece rule_name) {
  for (BindingEnv* env = this; env; env = env->parent_) {
    if (const Rule* rule = env->LookupRuleCurrentScope(rule_name))
      return rule;
  }
  return NULL;
}

void Rule::AddBinding(const string& key, const EvalString& val) {
  binding_mask_ |= VariableNameSignature(key);
  bindings_[key] = val;
}

void Rule::AddBinding(const string& key, EvalString&& val) {
  binding_mask_ |= VariableNameSignature(key);
  bindings_[key] = std::move(val);
}

const EvalString* Rule::GetBinding(StringPiece key) const {
  if (!(binding_mask_ & VariableNameSignature(key)))
    return NULL;
  Bindings::const_iterator i = bindings_.find(key);
  if (i == bindings_.end())
    return NULL;
  return &i->second;
}

std::unique_ptr<Rule> Rule::Phony() {
  auto rule = std::unique_ptr<Rule>(new Rule("phony"));
  rule->phony_ = true;
  return rule;
}

bool Rule::IsPhony() const {
  return phony_;
}

// static
bool Rule::IsReservedBinding(StringPiece var) {
  return var == "command" ||
      var == "depfile" ||
      var == "dyndep" ||
      var == "description" ||
      var == "deps" ||
      var == "generator" ||
      var == "pool" ||
      var == "restat" ||
      var == "rspfile" ||
      var == "rspfile_content" ||
      var == "msvc_deps_prefix";
}

const map<string, std::unique_ptr<const Rule>, StringPieceLess>& BindingEnv::GetRules() const {
  return rules_;
}

std::string BindingEnv::LookupWithFallback(StringPiece var,
                                           const EvalString* eval,
                                           Env* env) {
  if (const StringPiece* value = Find(var))
    return value->AsString();

  if (eval)
    return eval->Evaluate(env);

  if (parent_)
    return parent_->LookupVariable(var);

  return "";
}

void BindingEnv::AppendWithFallback(StringPiece var, const EvalString* eval,
                                    Env* env, std::string* out) {
  if (const StringPiece* value = Find(var)) {
    out->append(value->str_, value->len_);
    return;
  }

  if (eval) {
    eval->EvaluateInto(env, out);
    return;
  }

  if (parent_)
    parent_->AppendVariable(var, out);
}

string EvalString::Evaluate(Env* env) const {
  if (parsed_.empty()) {
    return single_token_;
  }

  string result;
  EvaluateInto(env, &result);
  return result;
}

void EvalString::EvaluateInto(Env* env, std::string* out) const {
  if (parsed_.empty()) {
    out->append(single_token_);
    return;
  }

  for (TokenList::const_iterator i = parsed_.begin(); i != parsed_.end(); ++i) {
    if (i->second == RAW)
      out->append(i->first);
    else
      env->AppendVariable(i->first, out);
  }
}

void EvalString::AddText(StringPiece text) {
  if (parsed_.empty()) {
    single_token_.append(text.begin(), text.end());
  } else if (!parsed_.empty() && parsed_.back().second == RAW) {
    parsed_.back().first.append(text.begin(), text.end());
  } else {
    parsed_.push_back(std::make_pair(text.AsString(), RAW));
  }
}

void EvalString::AddSpecial(StringPiece text) {
  if (parsed_.empty() && !single_token_.empty()) {
    // Going from one to two tokens, so we can no longer apply
    // our single_token_ optimization and need to push everything
    // onto the vector.
    parsed_.push_back(std::make_pair(std::move(single_token_), RAW));
  }
  parsed_.push_back(std::make_pair(text.AsString(), SPECIAL));
}

string EvalString::Serialize() const {
  string result;
  if (parsed_.empty() && !single_token_.empty()) {
    result.append("[");
    result.append(single_token_);
    result.append("]");
  } else {
    for (const auto& pair : parsed_) {
      result.append("[");
      if (pair.second == SPECIAL)
        result.append("$");
      result.append(pair.first.begin(), pair.first.end());
      result.append("]");
    }
  }
  return result;
}

string EvalString::Unparse() const {
  string result;
  if (parsed_.empty() && !single_token_.empty()) {
    result.append(single_token_.begin(), single_token_.end());
  } else {
    for (TokenList::const_iterator i = parsed_.begin();
         i != parsed_.end(); ++i) {
      bool special = (i->second == SPECIAL);
      if (special)
        result.append("${");
      result.append(i->first.begin(), i->first.end());
      if (special)
        result.append("}");
    }
  }
  return result;
}
