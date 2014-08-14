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

#ifndef _NINJA_WATCHER_H
#define _NINJA_WATCHER_H

#include <assert.h>
#ifndef _WIN32
#include <time.h>
#endif
#include <map>
#include <set>
#include <string>

struct WatchResult {
  void KeyAdded(void* key);
  void KeyDeleted(void* key);
  void KeyChanged(void* key);

  typedef std::set<void*> key_set_type;
  key_set_type added_keys_, changed_keys_, deleted_keys_;

  bool Pending() const {
    return !added_keys_.empty() || !changed_keys_.empty() ||
           !deleted_keys_.empty();
  }

  void Reset();
};

class Watcher {
 public:
  virtual void AddPath(std::string path, void* key) = 0;
  virtual void WaitForEvents() = 0;

  WatchResult result_;
};

#ifdef __linux

#define HAS_NATIVE_WATCHER 1

class NativeWatcher : public Watcher {
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

  timespec timeout_, last_refresh_;

  void Refresh(const std::string& path, WatchedNode* node);

 public:
  NativeWatcher();
  ~NativeWatcher();

  int fd_;
  void AddPath(std::string path, void* key);
  void OnReady();

  timespec* Timeout();
  void WaitForEvents();
};

#elif !defined(_WIN32)

class NativeWatcher : public Watcher {
 public:
  int fd_;
  void OnReady() {
    assert(0 && "not implemented");
  }
  timespec* Timeout() {
    assert(0 && "not implemented");
    return 0;
  }
};

#else

class NativeWatcher : public Watcher {};

#endif

#endif  // _NINJA_WATCHER_H
