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

#include "thread_pool.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <signal.h>
#endif

#include "util.h"

namespace {

/// The process-wide pool of worker threads.  It is created on first use and
/// never destroyed: ninja exits with exit(), and idle workers just block.
struct ThreadPool {
  static ThreadPool& Get() {
    static ThreadPool* pool = new ThreadPool;
    return *pool;
  }

  size_t concurrency() const { return workers_ + 1; }

  void Push(std::function<void()> task, bool urgent = false) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (urgent)
        queue_.push_front(std::move(task));
      else
        queue_.push_back(std::move(task));
    }
    cond_.notify_one();
  }

  /// Run queued tasks on the calling thread until \a done returns true.
  template <typename Done>
  void RunUntil(Done done) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      cond_.wait(lock, [&] { return done() || !queue_.empty(); });
      if (done())
        return;
      std::function<void()> task = std::move(queue_.front());
      queue_.pop_front();
      lock.unlock();
      task();
      lock.lock();
    }
  }

  /// Wake up all threads waiting in RunUntil() to recheck their condition.
  void NotifyAll() {
    // Taking the lock orders this with the condition checks in RunUntil().
    std::lock_guard<std::mutex> lock(mutex_);
    cond_.notify_all();
  }

 private:
  ThreadPool() {
    int processors = GetProcessorCount();
    workers_ = processors > 1 ? processors - 1 : 0;
#ifndef _WIN32
    // Workers must not handle any signals: the main thread relies on
    // signals (e.g. SIGCHLD, SIGINT) interrupting its ppoll()/pselect().
    // Threads inherit the signal mask of their creator.
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
#endif
    // The first worker starts the others: that takes a while, and the
    // caller has tasks to queue meanwhile.
    if (workers_) {
      std::thread([this] {
        for (size_t i = 1; i < workers_; ++i)
          std::thread([this] { RunUntil([] { return false; }); }).detach();
        RunUntil([] { return false; });
      }).detach();
    }
#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &old, NULL);
#endif
  }

  size_t workers_;
  std::mutex mutex_;
  std::condition_variable cond_;
  std::deque<std::function<void()>> queue_;
};

}  // anonymous namespace

void TaskGroup::Run(std::function<void()> task, bool urgent) {
  pending_.fetch_add(1, std::memory_order_relaxed);
  ThreadPool::Get().Push(
      [this, task = std::move(task)] {
        task();
        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)
          ThreadPool::Get().NotifyAll();
      },
      urgent);
}

void TaskGroup::Wait() {
  ThreadPool::Get().RunUntil(
      [this] { return pending_.load(std::memory_order_acquire) == 0; });
}

void RunDetached(std::function<void()> task) {
  ThreadPool::Get().Push(std::move(task));
}

void RunTasksUntil(const std::function<bool()>& done) {
  ThreadPool::Get().RunUntil(done);
}

void NotifyTaskWaiters() {
  ThreadPool::Get().NotifyAll();
}

size_t TaskGroup::Concurrency() {
  return ThreadPool::Get().concurrency();
}

void ParallelFor(size_t count, size_t grain,
                 const std::function<void(size_t begin, size_t end)>& fn) {
  if (grain == 0)
    grain = 1;
  size_t chunks = (count + grain - 1) / grain;
  size_t threads = std::min(chunks, TaskGroup::Concurrency());
  if (threads <= 1) {
    if (count)
      fn(0, count);
    return;
  }
  // The caller only waits for the chunks that other threads are working on:
  // it doesn't run other tasks meanwhile (which could take much longer).
  // Helpers that start late find no chunks left, so they only use the
  // shared state.
  struct State {
    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};
    size_t count = 0;
    size_t grain = 0;
    const std::function<void(size_t begin, size_t end)>* fn = nullptr;
    std::mutex mutex;
    std::condition_variable cond;
  };
  auto state = std::make_shared<State>();
  state->count = count;
  state->grain = grain;
  state->fn = &fn;
  auto work = [](State* s) {
    for (;;) {
      size_t begin = s->next.fetch_add(s->grain, std::memory_order_relaxed);
      if (begin >= s->count)
        return;
      size_t end = std::min(begin + s->grain, s->count);
      (*s->fn)(begin, end);
      if (s->done.fetch_add(end - begin, std::memory_order_acq_rel) +
              (end - begin) == s->count) {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->cond.notify_all();
      }
    }
  };
  for (size_t i = 1; i < threads; ++i)
    RunDetached([state, work] { work(state.get()); });
  work(state.get());
  std::unique_lock<std::mutex> lock(state->mutex);
  state->cond.wait(lock, [&] {
    return state->done.load(std::memory_order_acquire) == count;
  });
}
