// Copyright 2012 Google Inc. All Rights Reserved.
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

#include "deps_log.h"

#include <assert.h>
#include <errno.h>

#include <algorithm>
#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#if defined(_MSC_VER) && (_MSC_VER < 1900)
typedef __int32 int32_t;
typedef unsigned __int32 uint32_t;
#endif
#endif

#include "disk_interface.h"
#include "graph.h"
#include "metrics.h"
#include "state.h"
#include "thread_pool.h"
#include "util.h"

using namespace std;

// The version is stored as 4 bytes after the signature and also serves as a
// byte order mark. Signature and version combined are 16 bytes long.
static const char kFileSignature[] = "# ninjadeps\n";
static const size_t kFileSignatureSize = sizeof(kFileSignature) - 1u;

static const int32_t kCurrentVersion = 4;

// Record size is currently limited to less than the full 32 bit, due to
// internal buffers having to have this size.
static constexpr size_t kMaxRecordSize = (1 << 19) - 1;

/// The contents of a deps log file, mapped into memory if possible.
struct DepsLog::LoadedFile {
  ~LoadedFile() {
#ifndef _WIN32
    if (mapped_)
      munmap(const_cast<char*>(data_), size_);
#endif
  }

  /// Returns false and sets errno on errors.
  bool Load(const std::string& path) {
#ifndef _WIN32
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      return false;
    struct stat st;
    if (fstat(fd, &st) < 0) {
      int saved_errno = errno;
      close(fd);
      errno = saved_errno;
      return false;
    }
    size_ = st.st_size;
    if (size_ > 0) {
      void* data = mmap(NULL, size_, PROT_READ, MAP_PRIVATE, fd, 0);
      if (data != MAP_FAILED) {
        close(fd);
        data_ = static_cast<const char*>(data);
        mapped_ = true;
        return true;
      }
    }
    close(fd);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
      return false;
    std::string contents;
    char buf[64 << 10];
    size_t len;
    while ((len = fread(buf, 1, sizeof(buf), f)) > 0)
      contents.append(buf, len);
    fclose(f);
    // Keep the data 4-byte aligned.
    buffer_.reset(new int[contents.size() / 4 + 1]);
    memcpy(buffer_.get(), contents.data(), contents.size());
    data_ = reinterpret_cast<const char*>(buffer_.get());
    size_ = contents.size();
    return true;
#else
    // Read the file into the buffer directly: going through stdio copies
    // it a few times, which takes long for big logs.
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
      DWORD win_err = GetLastError();
      errno = win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND
                  ? ENOENT
                  : EIO;
      return false;
    }
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(f, &file_size)) {
      CloseHandle(f);
      errno = EIO;
      return false;
    }
    // Keep the data 4-byte aligned.
    size_t capacity = static_cast<size_t>(file_size.QuadPart);
    buffer_.reset(new int[capacity / 4 + 1]);
    char* data = reinterpret_cast<char*>(buffer_.get());
    size_t used = 0;
    while (used < capacity) {
      DWORD len;
      DWORD request =
          static_cast<DWORD>(std::min<size_t>(capacity - used, 1 << 30));
      if (!::ReadFile(f, data + used, request, &len, NULL)) {
        CloseHandle(f);
        errno = EIO;
        return false;
      }
      if (len == 0)
        break;
      used += len;
    }
    CloseHandle(f);
    data_ = data;
    size_ = used;
    return true;
#endif
  }

  const char* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  const char* data_ = nullptr;
  size_t size_ = 0;
  bool mapped_ = false;
  std::unique_ptr<int[]> buffer_;
};

DepsLog::DepsLog() : needs_recompaction_(false), file_(NULL) {}

DepsLog::~DepsLog() {
  Close();
}

bool DepsLog::OpenForWrite(const string& path, string* err) {
  if (needs_recompaction_) {
    if (!Recompact(path, err))
      return false;
  }

  assert(!file_);
  file_path_ = path;  // we don't actually open the file right now, but will do
                      // so on the first write attempt
  return true;
}

bool DepsLog::RecordDeps(Node* node, TimeStamp mtime,
                         const vector<Node*>& nodes) {
  return RecordDeps(node, mtime, static_cast<int>(nodes.size()), nodes.data());
}

bool DepsLog::RecordDeps(Node* node, TimeStamp mtime, int node_count,
                         Node* const* nodes) {
  // Track whether there's any new data to be recorded.
  bool made_change = false;

  // Assign ids to all nodes that are missing one.
  if (node->id() < 0) {
    if (!RecordId(node))
      return false;
    made_change = true;
  }
  for (int i = 0; i < node_count; ++i) {
    if (nodes[i]->id() < 0) {
      if (!RecordId(nodes[i]))
        return false;
      made_change = true;
    }
  }

  // See if the new data is different than the existing data, if any.
  if (!made_change) {
    Deps* deps = GetDeps(node);
    if (!deps ||
        deps->mtime != mtime ||
        deps->node_count != node_count) {
      made_change = true;
    } else {
      for (int i = 0; i < node_count; ++i) {
        if (deps->nodes[i] != nodes[i]) {
          made_change = true;
          break;
        }
      }
    }
  }

  // Don't write anything if there's no new info.
  if (!made_change)
    return true;

  // Update on-disk representation.
  unsigned size = 4 * (1 + 2 + node_count);
  if (size > kMaxRecordSize) {
    errno = ERANGE;
    return false;
  }

  if (!OpenForWriteIfNeeded()) {
    return false;
  }
  size |= 0x80000000;  // Deps record: set high bit.
  if (fwrite(&size, 4, 1, file_) < 1)
    return false;
  int id = node->id();
  if (fwrite(&id, 4, 1, file_) < 1)
    return false;
  uint32_t mtime_part = static_cast<uint32_t>(mtime & 0xffffffff);
  if (fwrite(&mtime_part, 4, 1, file_) < 1)
    return false;
  mtime_part = static_cast<uint32_t>((mtime >> 32) & 0xffffffff);
  if (fwrite(&mtime_part, 4, 1, file_) < 1)
    return false;
  for (int i = 0; i < node_count; ++i) {
    id = nodes[i]->id();
    if (fwrite(&id, 4, 1, file_) < 1)
      return false;
  }
  if (fflush(file_) != 0)
    return false;

  // Update in-memory representation.
  int* ids = new int[node_count];
  for (int i = 0; i < node_count; ++i)
    ids[i] = nodes[i]->id();
  Deps* deps = new Deps(mtime, node_count, ids, &nodes_);
  deps->owned_ids.reset(ids);
  UpdateDeps(node->id(), deps);

  return true;
}

void DepsLog::Close() {
  OpenForWriteIfNeeded();  // create the file even if nothing has been recorded
  if (file_)
    fclose(file_);
  file_ = NULL;
}

/// stat() results for the nodes of a deps log, by id, computed on other
/// threads while the manifest is parsed.
struct DepsLog::EarlyStats {
  enum : uint8_t { kNone, kClaimed, kDone, kTaken };

  EarlyStats(std::vector<Node*> nodes, const DiskInterface* disk_interface)
      : nodes(std::move(nodes)), disk_interface(disk_interface),
        states(new std::atomic<uint8_t>[this->nodes.size()]),
        mtimes(new TimeStamp[this->nodes.size()]) {
    for (size_t i = 0; i < this->nodes.size(); ++i)
      states[i].store(kNone, std::memory_order_relaxed);
  }

  ~EarlyStats() {
    cancel.store(true, std::memory_order_relaxed);
    tasks.Wait();
  }

  /// stat() the next chunk of nodes, then queue the rest, so that other
  /// tasks (parsing the manifest) get to run in between.
  void StatSome() {
    const size_t kChunk = 256;
    size_t begin = next.fetch_add(kChunk, std::memory_order_relaxed);
    if (begin >= nodes.size() || cancel.load(std::memory_order_relaxed))
      return;
    size_t end = std::min(begin + kChunk, nodes.size());
    for (size_t id = begin; id < end; ++id) {
      uint8_t s = kNone;
      if (!states[id].compare_exchange_strong(s, kClaimed,
                                              std::memory_order_acquire))
        continue;
      std::string err;
      mtimes[id] = disk_interface->Stat(nodes[id]->path(), &err);
      states[id].store(kDone, std::memory_order_release);
    }
    tasks.Run([this] { StatSome(); });
  }

  TimeStamp Take(int id) {
    if (forgotten || static_cast<size_t>(id) >= nodes.size())
      return -1;
    uint8_t s = kNone;
    if (states[id].compare_exchange_strong(s, kTaken,
                                           std::memory_order_acquire))
      return -1;
    while (s == kClaimed)
      s = states[id].load(std::memory_order_acquire);
    // A failed stat() returned -1 as well.
    return s == kDone ? mtimes[id] : -1;
  }

  std::vector<Node*> nodes;
  const DiskInterface* disk_interface;
  std::unique_ptr<std::atomic<uint8_t>[]> states;
  std::unique_ptr<TimeStamp[]> mtimes;
  std::atomic<size_t> next{0};
  std::atomic<bool> cancel{false};
  bool forgotten = false;
  TaskGroup tasks;
};

/// The result of DepsLog::Read().
struct DepsLog::PendingLoad {
  std::unique_ptr<LoadedFile> file;
  /// The errno of loading the file, or 0.
  int load_errno = 0;
  bool valid_header = false;
  int32_t version = 0;

  /// The records that passed the checks that don't need the State.
  struct Record {
    const char* data;
    unsigned size;
    bool is_deps;
    /// The offset of the record in the file.
    long offset;
    /// The number of path records up to this one (included).
    unsigned path_count;
  };
  std::vector<Record> records;
  /// The nodes for path records (null for deps records), if CreateNodes()
  /// ran.
  std::vector<Node*> nodes;
  /// See StartEarlyStats().
  std::shared_ptr<EarlyStats> early_stats;
  /// Whether a record failed the checks, and the end of the last record
  /// before it.
  bool read_failed = false;
  long valid_size = 0;
};

void DepsLog::PendingLoadDeleter::operator()(PendingLoad* pending) const {
  delete pending;
}

namespace {

/// The records found by WalkRecords().
struct RecordWalk {
  std::vector<DepsLog::PendingLoad::Record> records;
  /// Where the walk stopped.
  const char* end = nullptr;
  /// Whether it stopped at a record that is not valid.
  bool failed = false;
};

/// Check the parts of the record at \a p (before \a end) that don't depend
/// on the other records.  Returns its size and type, or false.
bool CheckRecord(const char* p, const char* end, unsigned* size,
                 bool* is_deps) {
  if (end - p < 4)
    return false;
  memcpy(size, p, 4);
  *is_deps = (*size >> 31) != 0;
  *size &= 0x7FFFFFFF;
  if (*size > kMaxRecordSize || end - (p + 4) < (ptrdiff_t)*size)
    return false;
  if (*is_deps)
    return (*size % 4) == 0 && *size >= 12;
  // A path and its checksum.
  return *size > 4;
}

/// Walk the records from \a p, those that start before \a limit, adding
/// them to \a walk with the number of path records so far in that walk.
void WalkRecords(const char* data, const char* p, const char* limit,
                 const char* end, RecordWalk* walk) {
  unsigned path_count = 0;
  while (p < limit) {
    unsigned size;
    bool is_deps;
    if (!CheckRecord(p, end, &size, &is_deps)) {
      walk->failed = end - p >= 4;
      break;
    }
    path_count += !is_deps;
    walk->records.push_back(
        { p + 4, size, is_deps, static_cast<long>(p - data), path_count });
    p += 4 + size;
  }
  walk->end = p;
}

/// Find the first position in [\a begin, \a chunk_end) that starts a chain
/// of records that look valid (with consecutive checksums), or null.
const char* FindRecordChain(const char* begin, const char* chunk_end,
                            const char* end) {
  const int kChainLength = 8;
  for (const char* start = begin; start < chunk_end; start += 4) {
    const char* p = start;
    int length = 0;
    bool have_checksum = false;
    unsigned last_checksum = 0;
    for (; length < kChainLength && p != end; ++length) {
      unsigned size;
      bool is_deps;
      if (!CheckRecord(p, end, &size, &is_deps))
        break;
      if (!is_deps) {
        unsigned checksum;
        memcpy(&checksum, p + 4 + size - 4, 4);
        if (have_checksum && ~checksum != ~last_checksum + 1)
          break;
        have_checksum = true;
        last_checksum = checksum;
      }
      p += 4 + size;
    }
    if (length == kChainLength || p == end)
      return start;
  }
  return nullptr;
}

}  // namespace

DepsLog::PendingLoadPtr DepsLog::Read(const string& path) {
  PendingLoadPtr pending(new PendingLoad);
  pending->file.reset(new LoadedFile);
  if (!pending->file->Load(path)) {
    pending->load_errno = errno;
    return pending;
  }
  const char* data = pending->file->data();
  const size_t file_size = pending->file->size();
  const char* end = data + file_size;

  pending->valid_header = file_size >= kFileSignatureSize &&
                          !memcmp(data, kFileSignature, kFileSignatureSize);
  if (file_size >= kFileSignatureSize + 4)
    memcpy(&pending->version, data + kFileSignatureSize, 4);
  if (!pending->valid_header || pending->version != kCurrentVersion)
    return pending;

  // Records are 4-byte aligned, as is the start of the data.
  const char* first = data + kFileSignatureSize + 4;
  std::vector<PendingLoad::Record>& records = pending->records;
  // Finding a record needs the previous one: walking through a big log is
  // a chain of cache misses.  So chunks of the log are walked in parallel,
  // each from the first position where a few records in a row look valid,
  // and the walks are joined where the true chain of records (from the
  // start) meets them.
  const size_t kMinChunkSize = 1 << 20;
  size_t chunk_count =
      std::min<size_t>(TaskGroup::Concurrency() * 2,
                       (end - first) / kMinChunkSize + 1);
  std::vector<RecordWalk> walks(chunk_count);
  size_t chunk_size = ((end - first) / chunk_count + 3) & ~size_t(3);
  ParallelFor(chunk_count, 1, [&](size_t begin, size_t end_chunk) {
    for (size_t i = begin; i < end_chunk; ++i) {
      const char* chunk_begin = first + i * chunk_size;
      const char* chunk_end = i + 1 == chunk_count
                                  ? end
                                  : first + (i + 1) * chunk_size;
      const char* start =
          i == 0 ? first : FindRecordChain(chunk_begin, chunk_end, end);
      if (start)
        WalkRecords(data, start, chunk_end, end, &walks[i]);
    }
  });

  // Join the walks.  The true chain is at p.  The records of the true chain
  // are segments of the walks, and records walked here, in order.
  struct Segment {
    const std::vector<PendingLoad::Record>* records;
    size_t begin, end;
    /// What to add to the records' path counts.
    unsigned path_offset;
  };
  std::vector<Segment> segments;
  RecordWalk walked;
  walked.records.reserve(chunk_count);
  const char* p = first;
  unsigned path_count = 0;
  bool failed = false;
  for (size_t i = 0; i < chunk_count && !failed && p != end; ++i) {
    const char* chunk_end =
        i + 1 == chunk_count ? end : first + (i + 1) * chunk_size;
    const RecordWalk& walk = walks[i];
    for (;;) {
      auto found = std::lower_bound(
          walk.records.begin(), walk.records.end(), p,
          [](const PendingLoad::Record& record, const char* pos) {
            return record.data - 4 < pos;
          });
      if (found != walk.records.end() && found->data - 4 == p) {
        // The chains meet: take the rest of this walk.
        unsigned before = found == walk.records.begin()
                              ? 0
                              : (found - 1)->path_count;
        segments.push_back({ &walk.records,
                             size_t(found - walk.records.begin()),
                             walk.records.size(), path_count - before });
        path_count += walk.records.back().path_count - before;
        p = walk.end;
        failed = walk.failed;
        break;
      }
      if (p >= chunk_end || p == end)
        break;
      // Walk the true chain by one record.
      RecordWalk step;
      WalkRecords(data, p, p + 1, end, &step);
      p = step.end;
      if (step.records.empty()) {
        failed = step.failed;
        break;
      }
      path_count += !step.records[0].is_deps;
      if (segments.empty() || segments.back().records != &walked.records)
        segments.push_back({ &walked.records, walked.records.size(),
                             walked.records.size(), 0 });
      walked.records.push_back(step.records[0]);
      walked.records.back().path_count = path_count;
      ++segments.back().end;
    }
  }
  pending->read_failed = failed;
  pending->valid_size = static_cast<long>(p - data);
  size_t record_count = 0;
  std::vector<size_t> segment_starts;
  for (const Segment& segment : segments) {
    segment_starts.push_back(record_count);
    record_count += segment.end - segment.begin;
  }
  records.resize(record_count);
  ParallelFor(segments.size(), 1, [&](size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
      const Segment& segment = segments[i];
      PendingLoad::Record* out = &records[segment_starts[i]];
      for (size_t j = segment.begin; j < segment.end; ++j, ++out) {
        *out = (*segment.records)[j];
        out->path_count += segment.path_offset;
      }
    }
  });

  // Check the path records' checksums: their expected index must match
  // their actual index. This can only happen if two ninja processes write to
  // the same deps log concurrently.  (This uses unary complement to make the
  // checksum look less like a dependency record entry.)
  // A truncated or concurrent write can also leave a deps record whose out_id
  // was never assigned a path.  Storing it made Recompact index nodes_ out of
  // bounds.  All ids must refer to paths that precede the record.  (Negative
  // ids become large as unsigned.)  Truncate at the first bad record.
  std::atomic<size_t> first_bad{records.size()};
  ParallelFor(records.size(), 1024, [&](size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
      const PendingLoad::Record& record = records[i];
      bool bad;
      if (record.is_deps) {
        const unsigned* ids = reinterpret_cast<const unsigned*>(record.data);
        unsigned max_id = ids[0];
        unsigned count = (record.size / 4) - 3;
        ids += 3;
        for (unsigned j = 0; j < count; ++j)
          max_id = std::max(max_id, ids[j]);
        bad = max_id >= record.path_count;
      } else {
        unsigned checksum;
        memcpy(&checksum, record.data + record.size - 4, 4);
        bad = ~checksum != record.path_count - 1;
      }
      if (bad) {
        size_t old = first_bad.load(std::memory_order_relaxed);
        while (i < old && !first_bad.compare_exchange_weak(old, i)) {
        }
        return;
      }
    }
  });
  if (first_bad < records.size()) {
    pending->read_failed = true;
    pending->valid_size = records[first_bad].offset;
    records.resize(first_bad);
  }
  return pending;
}

namespace {
StringPiece PathOfRecord(const char* buf, unsigned size) {
  int path_size = size - 4;
  // There can be up to 3 bytes of padding.
  if (buf[path_size - 1] == '\0') --path_size;
  if (buf[path_size - 1] == '\0') --path_size;
  if (buf[path_size - 1] == '\0') --path_size;
  return StringPiece(buf, path_size);
}
}  // namespace

void DepsLog::CreateNodes(PendingLoad* pending, State* state) {
  const std::vector<PendingLoad::Record>& records = pending->records;
  pending->nodes.resize(records.size());
  ParallelFor(records.size(), 1024, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      if (records[i].is_deps)
        continue;
      // The manifest may be parsed concurrently, so the nodes are created out
      // of order (and the manifest gets to set their slash bits).  See
      // Load() for the slash bits of the other ones.
      pending->nodes[i] = state->paths_.GetOrCreate(
          PathOfRecord(records[i].data, records[i].size), 0,
          /*provisional=*/true);
    }
  });
}

void DepsLog::StartEarlyStats(PendingLoad* pending,
                              const DiskInterface* disk_interface) {
  // The ids of the nodes, if loading the log succeeds.
  std::vector<Node*> nodes;
  for (Node* node : pending->nodes)
    if (node)
      nodes.push_back(node);
  pending->early_stats.reset(new EarlyStats(std::move(nodes), disk_interface));
  EarlyStats* early = pending->early_stats.get();
  // The tasks queue their next chunks behind other tasks.
  size_t tasks = TaskGroup::Concurrency();
  for (size_t i = 0; i < tasks; ++i)
    early->tasks.Run([early] { early->StatSome(); });
}

TimeStamp DepsLog::TakeEarlyStat(int id) {
  return early_stats_ ? early_stats_->Take(id) : -1;
}

void DepsLog::WaitForEarlyStats() {
  if (early_stats_)
    early_stats_->tasks.Wait();
}

void DepsLog::ForgetEarlyStats() {
  if (early_stats_) {
    early_stats_->forgotten = true;
    early_stats_->cancel.store(true, std::memory_order_relaxed);
    early_stats_->tasks.Wait();
  }
}

LoadStatus DepsLog::Load(const string& path, State* state, string* err) {
  return Load(Read(path), path, state, err);
}

bool DepsLog::LoadWithoutSideEffects(PendingLoadPtr* pending_ptr) {
  PendingLoad* pending = pending_ptr->get();
  if (pending->load_errno || !pending->valid_header ||
      pending->version != kCurrentVersion || pending->read_failed ||
      pending->nodes.empty() || !nodes_.empty() || !deps_.empty())
    return false;
  int unique_dep_record_count = 0;
  int total_dep_record_count = 0;
  // Create the Deps on all threads: that reads the records.
  const std::vector<PendingLoad::Record>& records = pending->records;
  std::vector<std::pair<int, Deps*> > created(records.size());
  ParallelFor(records.size(), 1024, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      const PendingLoad::Record& record = records[i];
      if (!record.is_deps)
        continue;
      const int* deps_data = reinterpret_cast<const int*>(record.data);
      TimeStamp mtime =
          (TimeStamp)(((uint64_t)(unsigned int)deps_data[2] << 32) |
                      (uint64_t)(unsigned int)deps_data[1]);
      created[i] = std::make_pair(
          deps_data[0],
          new Deps(mtime, (record.size / 4) - 3, deps_data + 3, &nodes_));
    }
  });
  for (size_t i = 0; i < records.size(); ++i) {
    const PendingLoad::Record& record = records[i];
    if (record.is_deps) {
      total_dep_record_count++;
      if (!UpdateDeps(created[i].first, created[i].second))
        ++unique_dep_record_count;
    } else {
      Node* node = pending->nodes[i];
      if (node->id() >= 0) {
        // A path appears twice: let Load() deal with that.  Undo.
        for (Node* n : nodes_)
          n->set_id(-1);
        nodes_.clear();
        for (Deps* deps : deps_)
          delete deps;
        deps_.clear();
        for (size_t j = i + 1; j < records.size(); ++j)
          delete created[j].second;
        return false;
      }
      node->set_id(static_cast<int>(nodes_.size()));
      nodes_.push_back(node);
    }
  }
  loaded_files_.push_back(std::move(pending->file));
  early_stats_ = std::move(pending->early_stats);
  pending_ptr->reset();

  // Rebuild the log if there are too many dead records.
  int kMinCompactionEntryCount = 1000;
  int kCompactionRatio = 3;
  if (total_dep_record_count > kMinCompactionEntryCount &&
      total_dep_record_count > unique_dep_record_count * kCompactionRatio) {
    needs_recompaction_ = true;
  }
  return true;
}

void DepsLog::UndoSpeculativeLoad(PendingLoad* pending, State* state) {
  std::vector<Node*> nodes;
  if (pending) {
    for (Node* node : pending->nodes)
      if (node)
        nodes.push_back(node);
  } else {
    nodes.swap(nodes_);
    for (Node* node : nodes)
      node->set_id(-1);
    for (Deps* deps : deps_)
      delete deps;
    deps_.clear();
    early_stats_.reset();
    needs_recompaction_ = false;
  }
  // Nodes that the manifest didn't refer to are still provisional.
  for (Node* node : nodes)
    if (node->slash_bits_provisional())
      state->paths_.Erase(node->path());
}

void DepsLog::FinishLoadWithoutSideEffects() {
  // As in Load().
  for (Node* node : nodes_)
    node->set_slash_bits_provisional(false);
}

LoadStatus DepsLog::Load(PendingLoadPtr pending, const string& path,
                         State* state, string* err) {
  METRIC_RECORD(".ninja_deps load");
  if (pending->load_errno) {
    if (pending->load_errno == ENOENT)
      return LOAD_NOT_FOUND;
    *err = strerror(pending->load_errno);
    return LOAD_ERROR;
  }

  // Note: For version differences, this should migrate to the new format.
  // But the v1 format could sometimes (rarely) end up with invalid data, so
  // don't migrate v1 to v3 to force a rebuild. (v2 only existed for a few days,
  // and there was no release with it, so pretend that it never happened.)
  if (!pending->valid_header || pending->version != kCurrentVersion) {
    if (pending->version == 1)
      *err = "deps log version change; rebuilding";
    else
      *err = "bad deps log signature or version; starting over";
    pending.reset();
    platformAwareUnlink(path.c_str());
    // Don't report this as a failure.  An empty deps log will cause
    // us to rebuild the outputs anyway.
    return LOAD_SUCCESS;
  }

  bool read_failed = pending->read_failed;
  long offset = pending->valid_size;
  int unique_dep_record_count = 0;
  int total_dep_record_count = 0;
  for (size_t i = 0; i < pending->records.size(); ++i) {
    const PendingLoad::Record& record = pending->records[i];
    const char* buf = record.data;
    unsigned size = record.size;
    if (record.is_deps) {
      const int* deps_data = reinterpret_cast<const int*>(buf);
      int out_id = deps_data[0];
      TimeStamp mtime;
      mtime = (TimeStamp)(((uint64_t)(unsigned int)deps_data[2] << 32) |
                          (uint64_t)(unsigned int)deps_data[1]);
      deps_data += 3;
      int deps_count = (size / 4) - 3;

      Deps* deps = new Deps(mtime, deps_count, deps_data, &nodes_);
      total_dep_record_count++;
      if (!UpdateDeps(out_id, deps))
        ++unique_dep_record_count;
    } else {
      // It is not necessary to pass in a correct slash_bits here. It will
      // either be a Node that's in the manifest (in which case it will already
      // have a correct slash_bits that GetNode will look up), or it is an
      // implicit dependency from a .d which does not affect the build command
      // (and so need not have its slashes maintained).
      Node* node;
      if (pending->nodes.empty()) {
        node = state->GetNode(PathOfRecord(buf, size), 0);
      } else {
        // If the manifest didn't refer to the node, its slash bits are the
        // ones it would have gotten from GetNode() now.
        node = pending->nodes[i];
        node->set_slash_bits_provisional(false);
      }

      // A path can only appear once (this also happens if two ninja
      // processes write to the same deps log concurrently).
      if (node->id() >= 0) {
        read_failed = true;
        offset = record.offset;
        break;
      }
      node->set_id(static_cast<int>(nodes_.size()));
      nodes_.push_back(node);
    }
  }
  loaded_files_.push_back(std::move(pending->file));
  early_stats_ = std::move(pending->early_stats);

  if (read_failed) {
    // An error occurred while loading; try to recover by truncating the
    // file to the last fully-read record.
    *err = "premature end of file";

    if (!Truncate(path, offset, err))
      return LOAD_ERROR;

    // The truncate succeeded; we'll just report the load error as a
    // warning because the build can proceed.
    *err += "; recovering";
    return LOAD_SUCCESS;
  }

  // Rebuild the log if there are too many dead records.
  int kMinCompactionEntryCount = 1000;
  int kCompactionRatio = 3;
  if (total_dep_record_count > kMinCompactionEntryCount &&
      total_dep_record_count > unique_dep_record_count * kCompactionRatio) {
    needs_recompaction_ = true;
  }

  return LOAD_SUCCESS;
}

DepsLog::Deps* DepsLog::GetDeps(Node* node) {
  // Abort if the node has no id (never referenced in the deps) or if
  // there's no deps recorded for the node.
  if (node->id() < 0 || node->id() >= (int)deps_.size())
    return NULL;
  return deps_[node->id()];
}

Node* DepsLog::GetFirstReverseDepsNode(Node* node) {
  for (size_t id = 0; id < deps_.size(); ++id) {
    Deps* deps = deps_[id];
    if (!deps)
      continue;
    if (id >= nodes_.size() || !nodes_[id])
      continue;
    for (int i = 0; i < deps->node_count; ++i) {
      if (deps->nodes[i] == node)
        return nodes_[id];
    }
  }
  return NULL;
}

bool DepsLog::Recompact(const string& path, string* err) {
  METRIC_RECORD(".ninja_deps recompact");

  Close();
  string temp_path = path + ".recompact";

  // OpenForWrite() opens for append.  Make sure it's not appending to a
  // left-over file from a previous recompaction attempt that crashed somehow.
  platformAwareUnlink(temp_path.c_str());

  DepsLog new_log;
  if (!new_log.OpenForWrite(temp_path, err))
    return false;

  // Clear all known ids so that new ones can be reassigned.  The new indices
  // will refer to the ordering in new_log, not in the current log.
  for (vector<Node*>::iterator i = nodes_.begin(); i != nodes_.end(); ++i)
    (*i)->set_id(-1);

  // Write out all deps again.
  for (int old_id = 0; old_id < (int)deps_.size(); ++old_id) {
    Deps* deps = deps_[old_id];
    if (!deps) continue;  // If nodes_[old_id] is a leaf, it has no deps.
    if (old_id >= (int)nodes_.size() || !nodes_[old_id])
      continue;

    if (!IsDepsEntryLiveFor(nodes_[old_id]))
      continue;

    std::vector<Node*> nodes(deps->node_count);
    for (int i = 0; i < deps->node_count; ++i)
      nodes[i] = deps->nodes[i];
    if (!new_log.RecordDeps(nodes_[old_id], deps->mtime, nodes)) {
      new_log.Close();
      return false;
    }
  }

  new_log.Close();

  // All nodes now have ids that refer to new_log, so steal its data.
  deps_.swap(new_log.deps_);
  nodes_.swap(new_log.nodes_);
  for (Deps* deps : deps_) {
    if (deps)
      deps->nodes.table = &nodes_;
  }

  return ReplaceContent(path, temp_path, err);
}

bool DepsLog::IsDepsEntryLiveFor(const Node* node) {
  // Skip entries that don't have in-edges or whose edges don't have a
  // "deps" attribute. They were in the deps log from previous builds, but
  // the the files they were for were removed from the build and their deps
  // entries are no longer needed.
  // (Without the check for "deps", a chain of two or more nodes that each
  // had deps wouldn't be collected in a single recompaction.)
  return node->in_edge() && !node->in_edge()->GetBinding("deps").empty();
}

bool DepsLog::UpdateDeps(int out_id, Deps* deps) {
  if (out_id >= (int)deps_.size())
    deps_.resize(out_id + 1);

  bool delete_old = deps_[out_id] != NULL;
  if (delete_old)
    delete deps_[out_id];
  deps_[out_id] = deps;
  return delete_old;
}

bool DepsLog::RecordId(Node* node) {
  int path_size = static_cast<int>(node->path().size());
  assert(path_size > 0 && "Trying to record empty path Node!");
  int padding = (4 - path_size % 4) % 4;  // Pad path to 4 byte boundary.

  unsigned size = path_size + padding + 4;
  if (size > kMaxRecordSize) {
    errno = ERANGE;
    return false;
  }

  if (!OpenForWriteIfNeeded()) {
    return false;
  }
  if (fwrite(&size, 4, 1, file_) < 1)
    return false;
  if (fwrite(node->path().data(), path_size, 1, file_) < 1) {
    return false;
  }
  if (padding && fwrite("\0\0", padding, 1, file_) < 1)
    return false;
  int id = static_cast<int>(nodes_.size());
  unsigned checksum = ~(unsigned)id;
  if (fwrite(&checksum, 4, 1, file_) < 1)
    return false;
  if (fflush(file_) != 0)
    return false;

  node->set_id(id);
  nodes_.push_back(node);

  return true;
}

bool DepsLog::OpenForWriteIfNeeded() {
  if (file_path_.empty()) {
    return true;
  }
  file_ = fopen(file_path_.c_str(), "ab");
  if (!file_) {
    return false;
  }
  // Set the buffer size to this and flush the file buffer after every record
  // to make sure records aren't written partially.
  if (setvbuf(file_, NULL, _IOFBF, kMaxRecordSize + 1) != 0) {
    return false;
  }
  SetCloseOnExec(fileno(file_));

  // Opening a file in append mode doesn't set the file pointer to the file's
  // end on Windows. Do that explicitly.
  fseek(file_, 0, SEEK_END);

  if (ftell(file_) == 0) {
    if (fwrite(kFileSignature, sizeof(kFileSignature) - 1, 1, file_) < 1) {
      return false;
    }
    if (fwrite(&kCurrentVersion, 4, 1, file_) < 1) {
      return false;
    }
  }
  if (fflush(file_) != 0) {
    return false;
  }
  file_path_.clear();
  return true;
}
