// Copyright 2014 Google Inc. All Rights Reserved.
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

#include "watcher.h"

void WatchResult::KeyAdded(void* key) {
  key_set_type::iterator i = deleted_keys_.find(key);
  if (i != deleted_keys_.end()) {
    deleted_keys_.erase(i);
    changed_keys_.insert(key);
  } else {
    added_keys_.insert(key);
  }
}

void WatchResult::KeyChanged(void* key) {
  if (added_keys_.find(key) == added_keys_.end()) changed_keys_.insert(key);
}

void WatchResult::KeyDeleted(void* key) {
  key_set_type::iterator i = added_keys_.find(key);
  if (i == added_keys_.end()) {
    changed_keys_.erase(key);
    deleted_keys_.insert(key);
  } else {
    added_keys_.erase(i);
  }
}

void WatchResult::Reset() {
  added_keys_.clear();
  changed_keys_.clear();
  deleted_keys_.clear();
}
