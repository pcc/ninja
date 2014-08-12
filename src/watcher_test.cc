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

#include "test.h"

#include <stdio.h>
#ifndef _WIN32
#include <unistd.h>
#endif

struct WatcherTest : public testing::Test {
  RealDiskInterface disk_;
  Watcher* watcher_;

  ScopedTempDir dir_;

  void SetUp() {
    disk_.CreateWatcher();
    watcher_ = disk_.GetWatcher();
    dir_.CreateAndEnter("watcher");
  }

  void TearDown() {
    dir_.Cleanup();
  }

  void *CharKey(char c) {
    return reinterpret_cast<void *>(c);
  }

  bool TouchFile(const char *path) {
    FILE* f = fopen(path, "w");
    fclose(f);
    return f;
  }
};

TEST_F(WatcherTest, Add) {
  watcher_->AddPath("a", CharKey('a'));

  ASSERT_TRUE(TouchFile("a"));

  watcher_->WaitForEvents();

  EXPECT_EQ(0, watcher_->changed_keys_.size());
  EXPECT_EQ(0, watcher_->deleted_keys_.size());
  ASSERT_EQ(1, watcher_->added_keys_.size());
  EXPECT_EQ(CharKey('a'), *watcher_->added_keys_.begin());
}

TEST_F(WatcherTest, Change) {
  ASSERT_TRUE(TouchFile("a"));

  watcher_->AddPath("a", CharKey('a'));

  ASSERT_TRUE(TouchFile("a"));

  watcher_->WaitForEvents();

  EXPECT_EQ(0, watcher_->added_keys_.size());
  EXPECT_EQ(0, watcher_->deleted_keys_.size());
  ASSERT_EQ(1, watcher_->changed_keys_.size());
  EXPECT_EQ(CharKey('a'), *watcher_->changed_keys_.begin());
}

TEST_F(WatcherTest, ChangeAbsolutePath) {
  ASSERT_TRUE(TouchFile("a"));

  watcher_->AddPath(dir_.start_dir_ + "/" + dir_.temp_dir_name_ + "/a",
                    CharKey('a'));

  ASSERT_TRUE(TouchFile("a"));

  watcher_->WaitForEvents();

  EXPECT_EQ(0, watcher_->added_keys_.size());
  EXPECT_EQ(0, watcher_->deleted_keys_.size());
  ASSERT_EQ(1, watcher_->changed_keys_.size());
  EXPECT_EQ(CharKey('a'), *watcher_->changed_keys_.begin());
}

TEST_F(WatcherTest, Delete) {
  ASSERT_TRUE(TouchFile("a"));

  watcher_->AddPath("a", CharKey('a'));

  remove("a");

  watcher_->WaitForEvents();

  EXPECT_EQ(0, watcher_->added_keys_.size());
  EXPECT_EQ(0, watcher_->changed_keys_.size());
  ASSERT_EQ(1, watcher_->deleted_keys_.size());
  EXPECT_EQ(CharKey('a'), *watcher_->deleted_keys_.begin());
}

TEST_F(WatcherTest, Rename) {
  ASSERT_TRUE(TouchFile("a"));

  watcher_->AddPath("a", CharKey('a'));
  watcher_->AddPath("b", CharKey('b'));

  ASSERT_EQ(0, rename("a", "b"));

  watcher_->WaitForEvents();

  EXPECT_EQ(0, watcher_->changed_keys_.size());
  ASSERT_EQ(1, watcher_->added_keys_.size());
  ASSERT_EQ(1, watcher_->deleted_keys_.size());
  EXPECT_EQ(CharKey('a'), *watcher_->deleted_keys_.begin());
  EXPECT_EQ(CharKey('b'), *watcher_->added_keys_.begin());
}

TEST_F(WatcherTest, AddSubdir1) {
  watcher_->AddPath("dir/a", CharKey('a'));
  watcher_->AddPath("b", CharKey('a'));

  ASSERT_TRUE(disk_.MakeDir("dir"));
  ASSERT_TRUE(TouchFile("dir/a"));

  watcher_->WaitForEvents();

  EXPECT_EQ(0, watcher_->changed_keys_.size());
  EXPECT_EQ(0, watcher_->deleted_keys_.size());
  ASSERT_EQ(1, watcher_->added_keys_.size());
  EXPECT_EQ(CharKey('a'), *watcher_->added_keys_.begin());
}

TEST_F(WatcherTest, AddSubdir2) {
  watcher_->AddPath("dir/a", CharKey('a'));
  watcher_->AddPath("b", CharKey('b'));

  ASSERT_TRUE(disk_.MakeDir("dir"));
  ASSERT_TRUE(TouchFile("b"));

  watcher_->WaitForEvents();

  EXPECT_EQ(0, watcher_->changed_keys_.size());
  EXPECT_EQ(0, watcher_->deleted_keys_.size());
  ASSERT_EQ(1, watcher_->added_keys_.size());
  EXPECT_EQ(CharKey('b'), *watcher_->added_keys_.begin());
  watcher_->Reset();

  ASSERT_TRUE(TouchFile("dir/a"));

  watcher_->WaitForEvents();

  EXPECT_EQ(0, watcher_->changed_keys_.size());
  EXPECT_EQ(0, watcher_->deleted_keys_.size());
  ASSERT_EQ(1, watcher_->added_keys_.size());
  EXPECT_EQ(CharKey('a'), *watcher_->added_keys_.begin());
}

TEST_F(WatcherTest, RenameSubdir) {
  watcher_->AddPath("dira/a", CharKey('a'));
  watcher_->AddPath("dirb/a", CharKey('b'));

  ASSERT_TRUE(disk_.MakeDir("dira"));
  ASSERT_TRUE(TouchFile("dira/a"));

  watcher_->WaitForEvents();

  EXPECT_EQ(0, watcher_->changed_keys_.size());
  EXPECT_EQ(0, watcher_->deleted_keys_.size());
  ASSERT_EQ(1, watcher_->added_keys_.size());
  EXPECT_EQ(CharKey('a'), *watcher_->added_keys_.begin());
  watcher_->Reset();

  ASSERT_EQ(0, rename("dira", "dirb"));

  watcher_->WaitForEvents();

  EXPECT_EQ(0, watcher_->changed_keys_.size());
  ASSERT_EQ(1, watcher_->added_keys_.size());
  ASSERT_EQ(1, watcher_->deleted_keys_.size());
  EXPECT_EQ(CharKey('a'), *watcher_->deleted_keys_.begin());
  EXPECT_EQ(CharKey('b'), *watcher_->added_keys_.begin());
}

#ifndef _WIN32
TEST_F(WatcherTest, Symlinks) {
  ASSERT_TRUE(disk_.MakeDir("dir"));
  ASSERT_EQ(0, symlink("dir", "link"));

  watcher_->AddPath("dir/a", CharKey('a'));
  watcher_->AddPath("link/b", CharKey('b'));

  ASSERT_TRUE(TouchFile("dir/a"));

  watcher_->WaitForEvents();

  EXPECT_EQ(0, watcher_->changed_keys_.size());
  EXPECT_EQ(0, watcher_->deleted_keys_.size());
  ASSERT_EQ(1, watcher_->added_keys_.size());
  EXPECT_EQ(CharKey('a'), *watcher_->added_keys_.begin());
  watcher_->Reset();

  ASSERT_TRUE(TouchFile("dir/b"));

  watcher_->WaitForEvents();

  EXPECT_EQ(0, watcher_->changed_keys_.size());
  EXPECT_EQ(0, watcher_->deleted_keys_.size());
  ASSERT_EQ(1, watcher_->added_keys_.size());
  EXPECT_EQ(CharKey('b'), *watcher_->added_keys_.begin());
}
#endif
