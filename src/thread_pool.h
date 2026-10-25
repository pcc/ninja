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

#ifndef NINJA_THREAD_POOL_H_
#define NINJA_THREAD_POOL_H_

#include <stddef.h>

#include <atomic>
#include <functional>

/// A group of tasks running on the process-wide thread pool.  Tasks may
/// themselves add more tasks to the group (or to other groups).
///
/// Wait() runs queued tasks on the calling thread while it waits, so
/// tasks may wait for nested groups without deadlocking the pool.
struct TaskGroup {
  TaskGroup() = default;
  TaskGroup(const TaskGroup&) = delete;
  TaskGroup& operator=(const TaskGroup&) = delete;
  ~TaskGroup() { Wait(); }

  /// Queue \a task for execution on some thread.
  void Run(std::function<void()> task);

  /// Wait for all tasks in the group, including tasks added by tasks, to
  /// finish.
  void Wait();

  /// The total number of threads that may run tasks, including the caller
  /// of Wait().
  static size_t Concurrency();

 private:
  std::atomic<size_t> pending_{0};
};

/// Run \a fn(begin, end) over subranges of [0, \a count) of about \a grain
/// items on the thread pool, and wait for all of them.
void ParallelFor(size_t count, size_t grain,
                 const std::function<void(size_t begin, size_t end)>& fn);

#endif  // NINJA_THREAD_POOL_H_
