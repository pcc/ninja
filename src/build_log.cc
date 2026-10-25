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

// On AIX, inttypes.h gets indirectly included by build_log.h.
// It's easiest just to ask for the printf format macros right away.
#ifndef _WIN32
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#endif

#include "build_log.h"
#include "disk_interface.h"

#include <cassert>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <inttypes.h>
#include <unistd.h>
#endif

#include "build.h"
#include "char_scan.h"
#include "graph.h"
#include "metrics.h"
#include "thread_pool.h"
#include "util.h"
#if defined(_MSC_VER) && (_MSC_VER < 1800)
#define strtoll _strtoi64
#endif

// Implementation details:
// Each run's log appends to the log file.
// To load, we run through all log entries in series, throwing away
// older runs.
// Once the number of redundant entries exceeds a threshold, we write
// out a new file and replace the existing one with it.

namespace {

const char kFileSignature[] = "# ninja log v%d\n";
const int kOldestSupportedVersion = 7;
const int kCurrentVersion = 7;

}  // namespace

// static
uint64_t BuildLog::LogEntry::HashCommand(StringPiece command) {
  return rapidhash(command.str_, command.len_);
}

BuildLog::LogEntry::LogEntry(std::string output) : output(std::move(output)) {}

BuildLog::LogEntry::LogEntry(const std::string& output, uint64_t command_hash,
                             int start_time, int end_time, TimeStamp mtime)
    : output(output), command_hash(command_hash), start_time(start_time),
      end_time(end_time), mtime(mtime) {}

BuildLog::BuildLog() = default;

BuildLog::~BuildLog() {
  Close();
}

bool BuildLog::OpenForWrite(const std::string& path, const BuildLogUser& user,
                            std::string* err) {
  if (needs_recompaction_) {
    if (!Recompact(path, user, err))
      return false;
  }

  assert(!log_file_);
  log_file_path_ = path;  // we don't actually open the file right now, but will
                          // do so on the first write attempt
  return true;
}

bool BuildLog::RecordCommand(Edge* edge, int start_time, int end_time,
                             TimeStamp mtime) {
  std::string command = edge->EvaluateCommand(true);
  uint64_t command_hash = LogEntry::HashCommand(command);
  for (std::vector<Node*>::iterator out = edge->outputs_.begin();
       out != edge->outputs_.end(); ++out) {
    const std::string& path = (*out)->path();
    Entries::iterator i = entries_.find(path);
    LogEntry* log_entry;
    if (i != entries_.end()) {
      log_entry = i->second.get();
    } else {
      log_entry = new LogEntry(path);
      // Passes ownership of |log_entry| to the map, but keeps the pointer valid.
      entries_.emplace(log_entry->output, std::unique_ptr<LogEntry>(log_entry));
    }
    log_entry->command_hash = command_hash;
    log_entry->start_time = start_time;
    log_entry->end_time = end_time;
    log_entry->mtime = mtime;

    if (!OpenForWriteIfNeeded()) {
      return false;
    }
    if (log_file_) {
      if (!WriteEntry(log_file_, *log_entry))
        return false;
      if (fflush(log_file_) != 0) {
          return false;
      }
    }
  }
  return true;
}

void BuildLog::Close() {
  OpenForWriteIfNeeded();  // create the file even if nothing has been recorded
  if (log_file_)
    fclose(log_file_);
  log_file_ = NULL;
}

bool BuildLog::OpenForWriteIfNeeded() {
  if (log_file_ || log_file_path_.empty()) {
    return true;
  }
  log_file_ = fopen(log_file_path_.c_str(), "ab");
  if (!log_file_) {
    return false;
  }
  if (setvbuf(log_file_, NULL, _IOLBF, BUFSIZ) != 0) {
    return false;
  }
  SetCloseOnExec(fileno(log_file_));

  // Opening a file in append mode doesn't set the file pointer to the file's
  // end on Windows. Do that explicitly.
  fseek(log_file_, 0, SEEK_END);

  if (ftell(log_file_) == 0) {
    if (fprintf(log_file_, kFileSignature, kCurrentVersion) < 0) {
      return false;
    }
  }
  return true;
}

namespace {

/// Lines at least this long are ignored (they used to not fit in a buffer).
const size_t kMaxLineSize = 256 << 10;

/// Parse a decimal integer the way strtoll() does, quickly in the usual
/// case.
int64_t ParseDecimal(const char* begin, const char* end) {
  const char* p = begin;
  bool negative = p != end && *p == '-';
  p += negative;
  uint64_t value = 0;
  // 19 digits fit in 64 bits.
  if (end - p <= 19) {
    for (; p != end && static_cast<unsigned>(*p - '0') < 10; ++p)
      value = value * 10 + (*p - '0');
  }
  if (p == end && value <= static_cast<uint64_t>(INT64_MAX))
    return negative ? -static_cast<int64_t>(value) : value;
  std::string copy(begin, end);
  return strtoll(copy.c_str(), NULL, 10);
}

/// The values of hexadecimal digits, or 16 for other characters.
struct HexDigits {
  HexDigits() {
    memset(values, 16, sizeof(values));
    for (int i = 0; i < 10; ++i)
      values['0' + i] = static_cast<uint8_t>(i);
    for (int i = 0; i < 6; ++i)
      values['a' + i] = values['A' + i] = static_cast<uint8_t>(10 + i);
  }
  uint8_t values[256];
};
const HexDigits kHexDigits;

/// Parse a hexadecimal integer the way strtoull() does, quickly in the
/// usual case.
uint64_t ParseHex(const char* begin, const char* end) {
  const char* p = begin;
  uint64_t value = 0;
  if (end - p <= 16) {
    for (; p != end; ++p) {
      unsigned digit = kHexDigits.values[static_cast<unsigned char>(*p)];
      if (digit == 16)
        break;
      value = value * 16 + digit;
    }
  }
  if (p == end && p != begin)
    return value;
  std::string copy(begin, end);
  return strtoull(copy.c_str(), NULL, 16);
}

/// An entry of the log, as parsed.
struct ParsedEntry {
  StringPiece output;
  int start_time;
  int end_time;
  TimeStamp mtime;
  uint64_t command_hash;
};

/// Parse the complete lines in [\a p, \a end).  The data ends with NULs
/// (see FindFirstOf()).
void ParseLines(const char* p, const char* end,
                std::vector<ParsedEntry>* entries) {
  while (p < end) {
    // The fields are separated by tabs.
    const char* start = p;
    const char* fields[5];
    int count = 0;
    for (;; ++p) {
      p = FindFirstOf<'\t', '\n', '\0'>(p);
      if (p >= end || *p == '\n')
        break;
      if (*p == '\t' && count < 4)
        fields[count++] = p;
    }
    if (p >= end)
      break;  // A line without a newline.
    const char* line_end = p++;
    if (count < 4 || static_cast<size_t>(line_end - start) >= kMaxLineSize)
      continue;
    ParsedEntry entry;
    entry.start_time = static_cast<int>(ParseDecimal(start, fields[0]));
    entry.end_time =
        static_cast<int>(ParseDecimal(fields[0] + 1, fields[1]));
    entry.mtime = ParseDecimal(fields[1] + 1, fields[2]);
    entry.output = StringPiece(fields[2] + 1, fields[3] - fields[2] - 1);
    // The rest of the line (which may have tabs) is the hash.
    entry.command_hash = ParseHex(fields[3] + 1, line_end);
    entries->push_back(entry);
  }
}

}  // namespace

LoadStatus BuildLog::Load(const std::string& path, std::string* err,
                          bool may_modify) {
  METRIC_RECORD(".ninja_log load");
  RealDiskInterface disk;
  FileContents contents;
  switch (disk.LoadFile(path, kCharScanPadding, &contents, err)) {
  case FileReader::Okay:
    break;
  case FileReader::NotFound:
    err->clear();
    return LOAD_NOT_FOUND;
  default:
    // A file that can be opened but not read (a directory) reads as empty.
    if (FILE* file = fopen(path.c_str(), "r")) {
      fclose(file);
      err->clear();
      return LOAD_SUCCESS;
    }
    return LOAD_ERROR;
  }
  if (contents.size == 0)
    return LOAD_SUCCESS;  // file was empty
  const char* data = contents.data;
  const char* end = data + contents.size;

  int log_version = 0;
  const char* first_line_end =
      static_cast<const char*>(memchr(data, '\n', end - data));
  std::string first_line(data, std::min<size_t>(
      (first_line_end ? first_line_end + 1 : end) - data, 64));
  sscanf(first_line.c_str(), kFileSignature, &log_version);
  bool invalid_log_version = false;
  if (log_version < kOldestSupportedVersion) {
    invalid_log_version = true;
    *err = "build log version is too old; starting over";
  } else if (log_version > kCurrentVersion) {
    invalid_log_version = true;
    *err = "build log version is too new; starting over";
  }
  if (invalid_log_version) {
    if (!may_modify)
      return LOAD_ERROR;
    platformAwareUnlink(path.c_str());
    // Don't report this as a failure. A missing build log will cause
    // us to rebuild the outputs anyway.
    return LOAD_NOT_FOUND;
  }

  // Parse chunks of lines in parallel.
  const size_t kMinChunkSize = 64 << 10;
  size_t chunk_count = std::min<size_t>(TaskGroup::Concurrency() * 2,
                                        contents.size / kMinChunkSize + 1);
  // Chunks end after a newline.
  std::vector<const char*> bounds(chunk_count + 1, end);
  bounds[0] = data;
  for (size_t i = 1; i < chunk_count; ++i) {
    const char* p =
        std::max(bounds[i - 1], data + contents.size * i / chunk_count);
    const char* line_end =
        static_cast<const char*>(memchr(p, '\n', end - p));
    bounds[i] = line_end ? line_end + 1 : end;
  }
  std::vector<std::vector<ParsedEntry> > parsed(chunk_count);
  std::vector<std::vector<LogEntry*> > created(chunk_count);
  ParallelFor(chunk_count, 1, [&](size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
      ParseLines(bounds[i], bounds[i + 1], &parsed[i]);
      created[i].reserve(parsed[i].size());
      for (const ParsedEntry& entry : parsed[i]) {
        created[i].push_back(new LogEntry(entry.output.AsString(),
                                          entry.command_hash,
                                          entry.start_time, entry.end_time,
                                          entry.mtime));
      }
    }
  });

  // Later entries for an output replace earlier ones.
  int unique_entry_count = 0;
  int total_entry_count = 0;
  size_t size = 0;
  for (const auto& chunk : created)
    size += chunk.size();
  entries_.reserve(static_cast<Entries::size_type>(entries_.size() + size));
  for (const auto& chunk : created) {
    for (LogEntry* entry : chunk) {
      ++total_entry_count;
      // The map only takes the entry if it's new.
      std::unique_ptr<LogEntry> owned(entry);
      auto inserted = entries_.emplace(entry->output, std::move(owned));
      if (inserted.second) {
        ++unique_entry_count;
      } else {
        LogEntry* old = inserted.first->second.get();
        old->start_time = entry->start_time;
        old->end_time = entry->end_time;
        old->mtime = entry->mtime;
        old->command_hash = entry->command_hash;
      }
    }
  }

  // Decide whether it's time to rebuild the log:
  // - if we're upgrading versions
  // - if it's getting large
  int kMinCompactionEntryCount = 100;
  int kCompactionRatio = 3;
  if (log_version < kCurrentVersion) {
    needs_recompaction_ = true;
  } else if (total_entry_count > kMinCompactionEntryCount &&
             total_entry_count > unique_entry_count * kCompactionRatio) {
    needs_recompaction_ = true;
  }

  return LOAD_SUCCESS;
}

BuildLog::LogEntry* BuildLog::LookupByOutput(const std::string& path) const{
  Entries::const_iterator i = entries_.find(path);
  if (i != entries_.end())
    return i->second.get();
  return NULL;
}

bool BuildLog::WriteEntry(FILE* f, const LogEntry& entry) {
  return fprintf(f, "%d\t%d\t%" PRId64 "\t%s\t%" PRIx64 "\n",
          entry.start_time, entry.end_time, entry.mtime,
          entry.output.c_str(), entry.command_hash) > 0;
}

bool BuildLog::Recompact(const std::string& path, const BuildLogUser& user,
                         std::string* err) {
  METRIC_RECORD(".ninja_log recompact");

  Close();
  std::string temp_path = path + ".recompact";
  FILE* f = fopen(temp_path.c_str(), "wb");
  if (!f) {
    *err = strerror(errno);
    return false;
  }

  if (fprintf(f, kFileSignature, kCurrentVersion) < 0) {
    *err = strerror(errno);
    fclose(f);
    return false;
  }

  std::vector<StringPiece> dead_outputs;
  for (const auto& pair : entries_) {
    if (user.IsPathDead(pair.first)) {
      dead_outputs.push_back(pair.first);
      continue;
    }

    if (!WriteEntry(f, *pair.second)) {
      *err = strerror(errno);
      fclose(f);
      return false;
    }
  }

  for (StringPiece output : dead_outputs)
    entries_.erase(output);

  fclose(f);

  return ReplaceContent(path, temp_path, err);
}

bool BuildLog::Restat(const StringPiece path,
                      const DiskInterface& disk_interface,
                      const int output_count, char** outputs,
                      std::string* const err) {
  METRIC_RECORD(".ninja_log restat");

  Close();
  std::string temp_path = path.AsString() + ".restat";
  FILE* f = fopen(temp_path.c_str(), "wb");
  if (!f) {
    *err = strerror(errno);
    return false;
  }

  if (fprintf(f, kFileSignature, kCurrentVersion) < 0) {
    *err = strerror(errno);
    fclose(f);
    return false;
  }
  for (auto& pair : entries_) {
    bool skip = output_count > 0;
    for (int j = 0; j < output_count; ++j) {
      if (pair.second->output == outputs[j]) {
        skip = false;
        break;
      }
    }
    if (!skip) {
      const TimeStamp mtime = disk_interface.Stat(pair.second->output, err);
      if (mtime == -1) {
        fclose(f);
        return false;
      }
      pair.second->mtime = mtime;
    }

    if (!WriteEntry(f, *pair.second)) {
      *err = strerror(errno);
      fclose(f);
      return false;
    }
  }

  fclose(f);

  return ReplaceContent(path.AsString(), temp_path, err);
}
