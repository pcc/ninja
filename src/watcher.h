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

#include <map>
#include <set>
#include <string>

#ifdef __linux
#define HAS_WATCHER 1

class Watcher {
  struct WatchedNode;

  struct WatchMapEntry {
    WatchMapEntry() {}
    WatchMapEntry(const std::string& path, WatchedNode* node)
        : path_(path), node_(node) {}
    std::string path_;
    WatchedNode* node_;
  };

  typedef std::map<int, WatchMapEntry> watch_map_type;
  watch_map_type watch_map_;

  typedef std::map<std::string, WatchedNode> subdir_map_type;

  struct WatchedNode {
    bool has_wd_;
    watch_map_type::iterator it_;
    void* key_;
    subdir_map_type subdirs_;
  };

  subdir_map_type roots_;

  typedef std::set<void*> key_set_type;
  key_set_type added_keys_, changed_keys_, deleted_keys_;

  timespec timeout_, last_refresh_;

  void KeyAdded(void* key);
  void KeyDeleted(void* key);
  void KeyChanged(void* key);
  void Refresh(const std::string& path, WatchedNode* node);

 public:
  Watcher();
  ~Watcher();

  int fd_;
  void AddPath(std::string path, void* key);
  void OnReady();

  typedef key_set_type::const_iterator key_set_iterator;
  key_set_iterator added_keys_begin() const { return added_keys_.begin(); }
  key_set_iterator added_keys_end() const { return added_keys_.end(); }
  key_set_iterator changed_keys_begin() const { return changed_keys_.begin(); }
  key_set_iterator changed_keys_end() const { return changed_keys_.end(); }
  key_set_iterator deleted_keys_begin() const { return deleted_keys_.begin(); }
  key_set_iterator deleted_keys_end() const { return deleted_keys_.end(); }
  void Reset();
  bool Pending() const {
    return !added_keys_.empty() || !changed_keys_.empty() ||
           !deleted_keys_.empty();
  }
  timespec *Timeout();
  void WaitForEvents();
};

#else  // !__linux

#define HAS_WATCHER 0

#endif
