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

#include "graph.h"

#include <assert.h>
#include <stdio.h>

#include "build_log.h"
#include "disk_interface.h"
#include "parsers.h"
#include "state.h"
#include "util.h"

bool FileStat::Stat(DiskInterface* disk_interface) {
  mtime_ = disk_interface->Stat(path_);
  return mtime_ > 0;
}

bool Edge::RecomputeDirty(State* state, DiskInterface* disk_interface,
                          string* err) {
  num_dirty_inputs_ = 0;

  if (!rule_->depfile_.empty()) {
    if (!LoadDepFile(state, disk_interface, err))
      return false;
  }

  time_t most_recent_input = 1;
  for (vector<Node*>::iterator i = inputs_.begin(); i != inputs_.end(); ++i) {
    if ((*i)->file_->StatIfNecessary(disk_interface)) {
      if (Edge* edge = (*i)->in_edge_) {
        if (!edge->RecomputeDirty(state, disk_interface, err))
          return false;
      } else {
        // This input has no in-edge; it is dirty if it is missing.
        (*i)->dirty_ = !(*i)->file_->exists();
      }
    }

    if (is_order_only(i - inputs_.begin())) {
      // Order-only deps only make us dirty if they're missing.
      if (!(*i)->file_->exists())
        num_dirty_inputs_++;
      continue;
    }

    // If a regular input is dirty (or missing), we're dirty.
    // Otherwise consider mtime.
    if ((*i)->dirty_) {
      num_dirty_inputs_++;
    } else {
      if ((*i)->file_->mtime_ > most_recent_input)
        most_recent_input = (*i)->file_->mtime_;
    }
  }

  string command = EvaluateCommand();

  assert(!outputs_.empty());
  for (vector<Node*>::iterator i = outputs_.begin(); i != outputs_.end(); ++i) {
    // We may have other outputs that our input-recursive traversal hasn't hit
    // yet (or never will).  Stat them if we haven't already to mark that we've
    // visited their dependents.
    (*i)->file_->StatIfNecessary(disk_interface);

    (*i)->dirty_ = IsOutputDirty(state ? state->build_log_ : 0, most_recent_input,
                                 command, *i);
  }
  return true;
}

bool Edge::IsOutputDirty(BuildLog* build_log, time_t most_recent_input,
                         const string& command, Node* output) {
  if (is_phony()) {
    // Phony edges don't write any output.
    // They're only dirty if an input is dirty.
    return num_dirty_inputs_ > 0;
  }

  // Output is dirty if we're dirty, we're missing the output,
  // or if it's older than the most recent input mtime.
  if (num_dirty_inputs_ > 0 || !output->file_->exists() ||
      output->file_->mtime_ < most_recent_input) {
    return true;
  } else {
    // May also be dirty due to the command changing since the last build.
    BuildLog::LogEntry* entry;
    if (build_log &&
        (entry = build_log->LookupByOutput(output->file_->path_))) {
      if (command != entry->command)
        return true;
    }
  }
  return false;
}

void Edge::CleanInput(BuildLog* build_log, Node* input,
                      set<Edge*>* touched_edges) {
  if (!outputs_[0]->file_->status_known())
    return;
  touched_edges->insert(this);

  size_t non_order_only_count =
    count(inputs_.begin(), inputs_.end() - order_only_deps_, input);
  if (non_order_only_count == 0)
    // This is an order-only dependency.  If we reach this point, the dependency
    // must have existed at the start.
    return;

  num_dirty_inputs_ -= non_order_only_count;
  if (num_dirty_inputs_ > 0)
    return;

  time_t most_recent_input = 1;
  for (vector<Node*>::iterator i = inputs_.begin(); i != inputs_.end() - order_only_deps_; ++i) {
    if ((*i)->file_->mtime_ > most_recent_input)
      most_recent_input = (*i)->file_->mtime_;
  }

  string command = EvaluateCommand();

  for (vector<Node*>::iterator i = outputs_.begin(); i != outputs_.end(); ++i) {
    if (!(*i)->dirty_)
      continue;

    if (!IsOutputDirty(build_log, most_recent_input, command, *i)) {
      (*i)->dirty_ = false;
      set<Edge*> out_edges((*i)->out_edges_.begin(), (*i)->out_edges_.end());
      for (set<Edge*>::iterator ei = out_edges.begin();
           ei != out_edges.end(); ++ei)
        (*ei)->CleanInput(build_log, *i, touched_edges);
    }
  }
}

/// An Env for an Edge, providing $in and $out.
struct EdgeEnv : public Env {
  EdgeEnv(Edge* edge) : edge_(edge) {}
  virtual string LookupVariable(const string& var) {
    string result;
    if (var == "in") {
      // Iterate through explicit deps only.
      for (vector<Node*>::iterator i = edge_->inputs_.begin(),
           e = edge_->inputs_.end() - edge_->implicit_deps_ -
               edge_->order_only_deps_; i != e; ++i) {
        if (!result.empty())
          result.push_back(' ');
        result.append((*i)->file_->path_);
      }
    } else if (var == "out") {
      for (vector<Node*>::iterator i = edge_->outputs_.begin();
           i != edge_->outputs_.end(); ++i) {
        if (!result.empty())
          result.push_back(' ');
        result.append((*i)->file_->path_);
      }
    } else if (edge_->env_) {
      return edge_->env_->LookupVariable(var);
    }
    return result;
  }
  Edge* edge_;
};

string Edge::EvaluateCommand() {
  EdgeEnv env(this);
  return rule_->command_.Evaluate(&env);
}

string Edge::GetDescription() {
  EdgeEnv env(this);
  return rule_->description_.Evaluate(&env);
}

bool Edge::LoadDepFile(State* state, DiskInterface* disk_interface,
                       string* err) {
  EdgeEnv env(this);
  string path = rule_->depfile_.Evaluate(&env);

  string content = disk_interface->ReadFile(path, err);
  if (!err->empty())
    return false;
  if (content.empty())
    return true;

  MakefileParser makefile;
  string makefile_err;
  if (!makefile.Parse(content, &makefile_err)) {
    *err = path + ": " + makefile_err;
    return false;
  }

  // Check that this depfile matches our output.
  if (outputs_[0]->file_->path_ != makefile.out_) {
    *err = "expected makefile to mention '" + outputs_[0]->file_->path_ + "', "
           "got '" + makefile.out_ + "'";
    return false;
  }

  // Add all its in-edges.
  for (vector<string>::iterator i = makefile.ins_.begin();
       i != makefile.ins_.end(); ++i) {
    CanonicalizePath(&*i);

    Node* node = state->GetNode(*i);
    inputs_.insert(inputs_.end() - order_only_deps_, node);
    node->out_edges_.push_back(this);
    ++implicit_deps_;

    // If we don't have a edge that generates this input already,
    // create one; this makes us not abort if the input is missing,
    // but instead will rebuild in that circumstance.
    if (!node->in_edge_) {
      Edge* phony_edge = state->AddEdge(&State::kPhonyRule);
      phony_edge->order_only_deps_ = 1;
      node->in_edge_ = phony_edge;
      phony_edge->outputs_.push_back(node);
    }
  }

  return true;
}

void Edge::Dump() {
  printf("[ ");
  for (vector<Node*>::iterator i = inputs_.begin(); i != inputs_.end(); ++i) {
    printf("%s ", (*i)->file_->path_.c_str());
  }
  printf("--%s-> ", rule_->name_.c_str());
  for (vector<Node*>::iterator i = outputs_.begin(); i != outputs_.end(); ++i) {
    printf("%s ", (*i)->file_->path_.c_str());
  }
  printf("]\n");
}

bool Edge::is_phony() const {
  return rule_ == &State::kPhonyRule;
}
