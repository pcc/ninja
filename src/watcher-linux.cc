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

#ifdef __linux

#include "watcher.h"
#include "util.h"
#include <errno.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

Watcher::Watcher() {
  fd_ = inotify_init();
  SetCloseOnExec(fd_);
}

Watcher::~Watcher() { close(fd_); }

void Watcher::AddPath(std::string path, void* key) {
  size_t pos = 0;
  subdir_map_type* map = &roots_;

  while (1) {
    size_t slash_offset = path.find('/', pos);
    std::string subdir = path.substr(pos, slash_offset - pos);
    WatchedNode* subdir_node = &(*map)[subdir];

    int mask = IN_CREATE | IN_MOVED_TO | IN_MOVE_SELF | IN_DELETE_SELF;
    if (slash_offset == std::string::npos) {
      mask = IN_CLOSE_WRITE | IN_MOVE_SELF | IN_DELETE_SELF;
      subdir_node->key_ = key;
    }

    if (!subdir_node->has_wd_ && slash_offset != 0) {
      std::string subpath = path.substr(0, slash_offset);
      int wd = inotify_add_watch(fd_, subpath.c_str(), mask);
      if (wd != -1) {
        std::pair<watch_map_type::iterator, bool> ins = watch_map_.insert(
            std::make_pair(wd, WatchMapEntry(subpath, subdir_node)));
        if (!ins.second) {
          // We are already watching this node through another path, e.g. via a
          // symlink. Rewrite path to use the existing path as a prefix.
          map->erase(subdir);
          if (slash_offset != std::string::npos) {
            path = ins.first->second.path_ + path.substr(slash_offset);
            slash_offset = ins.first->second.path_.size();
          }
          subdir_node = ins.first->second.node_;
        } else {
          subdir_node->it_ = ins.first;
          subdir_node->has_wd_ = true;
        }
      }
    }

    if (slash_offset == std::string::npos) {
      break;
    } else {
      pos = slash_offset + 1;
      map = &subdir_node->subdirs_;
    }
  }
}

void Watcher::OnReady() {
  // We may only read full structures out of this fd, and we have no way of
  // knowing how large they are. This makes reading this fd rather tricky.
  char* buf;
  size_t size = sizeof(inotify_event);
  while (1) {
    buf = new char[size];
    ssize_t ret = read(fd_, buf, size);
    if (ret != ssize_t(size)) {
      if (errno == EINVAL) {
        // Buffer too small. Increase by sizeof(inotify_event) to avoid reading
        // more than one event.
        size += sizeof(inotify_event);
        delete[] buf;
        continue;
      }
      Fatal("read: %s", strerror(errno));
    }
    break;
  }
  inotify_event* ev = reinterpret_cast<inotify_event*>(buf);

  if (ev->mask & IN_IGNORED) {
    watch_map_.erase(ev->wd);
    delete[] buf;
    return;
  }

  WatchMapEntry* wme = &watch_map_[ev->wd];
  if (!wme->node_) {
    // We've removed the watch, but we will continue to receive notifications
    // from before we removed it, which we can safely ignore.
    delete[] buf;
    return;
  }

  if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
    subdir_map_type::iterator i = wme->node_->subdirs_.find(ev->name);
    if (i != wme->node_->subdirs_.end()) {
      Refresh(wme->path_ + "/" + ev->name, &i->second);
    }
  }

  if (ev->mask & (IN_MOVE_SELF | IN_DELETE_SELF)) {
    Refresh(wme->path_, wme->node_);
  }

  if (ev->mask & IN_CLOSE_WRITE) {
    KeyChanged(wme->node_->key_);
  }

  delete[] buf;

  clock_gettime(CLOCK_MONOTONIC, &last_refresh_);
}

void Watcher::Refresh(const std::string& path, WatchedNode* node) {
  bool had_wd = node->has_wd_;
  if (had_wd) {
    inotify_rm_watch(fd_, node->it_->first);
    node->it_->second.node_ = 0;
    node->it_ = watch_map_type::iterator();
    node->has_wd_ = false;
  }

  int mask = IN_CREATE | IN_MOVED_TO | IN_MOVE_SELF | IN_DELETE_SELF;
  if (node->key_) mask = IN_CLOSE_WRITE | IN_MOVE_SELF | IN_DELETE_SELF;

  int wd = inotify_add_watch(fd_, path.c_str(), mask);
  if (wd != -1) {
    watch_map_[wd] = WatchMapEntry(path, node);
    node->it_ = watch_map_.find(wd);
    node->has_wd_ = true;
  }
  bool has_wd = node->has_wd_;

  if (node->key_) {
    if (had_wd && has_wd) {
      KeyChanged(node->key_);
    } else if (had_wd && !has_wd) {
      KeyDeleted(node->key_);
    } else if (!had_wd && has_wd) {
      KeyAdded(node->key_);
    }
  }

  for (subdir_map_type::iterator i = node->subdirs_.begin();
       i != node->subdirs_.end(); ++i) {
    Refresh(path + "/" + i->first, &i->second);
  }
}

void Watcher::KeyAdded(void* key) {
  key_set_type::iterator i = deleted_keys_.find(key);
  if (i != deleted_keys_.end()) {
    deleted_keys_.erase(i);
    changed_keys_.insert(key);
  } else {
    added_keys_.insert(key);
  }
}

void Watcher::KeyChanged(void* key) {
  if (added_keys_.find(key) == added_keys_.end()) changed_keys_.insert(key);
}

void Watcher::KeyDeleted(void* key) {
  key_set_type::iterator i = added_keys_.find(key);
  if (i == added_keys_.end()) {
    changed_keys_.erase(key);
    deleted_keys_.insert(key);
  } else {
    added_keys_.erase(i);
  }
}

void Watcher::Reset() {
  added_keys_.clear();
  changed_keys_.clear();
  deleted_keys_.clear();
}

timespec* Watcher::Timeout() {
  const long hysteresis_ns = 100000000;

  if (!Pending())
    return 0;

  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (now.tv_sec > last_refresh_.tv_sec+1) {
    timeout_.tv_sec = 0;
    timeout_.tv_nsec = 0;
    return &timeout_;
  }

  long now_ns = now.tv_nsec;
  if (now.tv_sec != last_refresh_.tv_sec)
    now_ns += 1000000000;
  if (now_ns > last_refresh_.tv_nsec + hysteresis_ns) {
    timeout_.tv_sec = 0;
    timeout_.tv_nsec = 0;
    return &timeout_;
  }

  timeout_.tv_sec = 0;
  timeout_.tv_nsec = last_refresh_.tv_nsec + hysteresis_ns - now_ns;
  return &timeout_;
}

#endif
