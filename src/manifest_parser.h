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

#ifndef NINJA_MANIFEST_PARSER_H_
#define NINJA_MANIFEST_PARSER_H_

#include "parser.h"

#include <functional>
#include <memory>
#include <vector>

struct BindingEnv;
struct EvalString;

enum DupeEdgeAction {
  kDupeEdgeActionWarn,
  kDupeEdgeActionError,
};

enum PhonyCycleAction {
  kPhonyCycleActionWarn,
  kPhonyCycleActionError,
};

struct ManifestParserOptions {
  PhonyCycleAction phony_cycle_action_ = kPhonyCycleActionWarn;
  /// If set, called once the top-level file (and its includes) has been
  /// processed, i.e. once the top-level variables are known (but while
  /// other files may still be parsed concurrently).
  std::function<void()> top_level_scope_done_;
};

/// Parses .ninja files.
struct ManifestParser : public Parser {
  ManifestParser(State* state, FileReader* file_reader,
                 ManifestParserOptions options = ManifestParserOptions());

  /// Load and parse a file.
  bool Load(const std::string& filename, std::string* err);

  /// Parse a text string of input.  Used by tests.
  bool ParseTest(const std::string& input, std::string* err) {
    quiet_ = true;
    return Parse("input", input, err);
  }

  /// Retrieve the expanded value of a top-level variable from the
  /// manifest. Returns an empty string if the variable is not defined.
  /// Must be called only after a successful Load() call.
  std::string LookupVariable(const std::string& varname);

 private:
  /// Parse a file, given its contents as a string.
  bool Parse(const std::string& filename, const std::string& input,
             std::string* err);

  /// Parse a file, given its contents (which this takes).
  bool ParseContents(const std::string& filename, std::string* contents,
                     std::string* err);
  /// Parse the top-level file (a ParsedFile, which this takes).
  bool ParseRoot(void* root_file, std::string* err);

  struct Merger;
  friend struct Merger;

  BindingEnv* env_;
  ManifestParserOptions options_;
  bool quiet_;
};

#endif  // NINJA_MANIFEST_PARSER_H_
