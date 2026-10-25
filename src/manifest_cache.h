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

#ifndef NINJA_MANIFEST_CACHE_H_
#define NINJA_MANIFEST_CACHE_H_

#include <stdint.h>

#include <string>

#include "manifest_parser.h"

struct State;

/// A cache of the result of parsing a manifest, kept in a file, so that the
/// manifest doesn't need to be parsed again as long as the files that make
/// it up don't change.
///
/// The cache records the status (see FileStatus) of the files that were
/// read, and is only used if they all still have the same status.  To
/// account for the granularity of timestamps, a cache isn't saved if any of
/// the files changed very recently, or while being read.
struct ManifestCache {
  /// Load the result of parsing \a input_file with \a options from the cache
  /// in the file \a path into \a state, which must be new (but nodes may be
  /// created in it concurrently, see DepsLog::CreateNodes()).  Returns false
  /// (without changing \a state) if the cache is missing or doesn't apply.
  static bool Load(const std::string& path, const std::string& input_file,
                   const ManifestParserOptions& options, State* state);

  /// Save the result of parsing \a input_file with \a options, which is in
  /// \a state, into the cache in the file \a path, if possible.  Must be
  /// called right after a successful parse that printed no warnings.
  static bool Save(const std::string& path, const std::string& input_file,
                   const ManifestParserOptions& options, const State& state,
                   std::string* err);

  /// Files whose status changed less than this many nanoseconds before
  /// saving are not trusted: they may change again without that being
  /// visible in their status, given the granularity of timestamps.  (Tests
  /// lower it.)
  static int64_t recent_change_ns;
};

#endif  // NINJA_MANIFEST_CACHE_H_
