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

#ifndef NINJA_DISK_INTERFACE_H_
#define NINJA_DISK_INTERFACE_H_

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "timestamp.h"

/// Interface for reading files from disk.  See DiskInterface for details.
/// This base offers the minimum interface needed just to read files.
/// The identity and status of a file, to tell whether it changed.
struct FileStatus {
  uint64_t dev = 0;
  uint64_t ino = 0;
  uint64_t size = 0;
  int64_t mtime = 0;
  int64_t ctime = 0;

  bool operator==(const FileStatus& other) const {
    return dev == other.dev && ino == other.ino && size == other.size &&
           mtime == other.mtime && ctime == other.ctime;
  }
  bool operator!=(const FileStatus& other) const { return !(*this == other); }

  /// Get the status of \a path.  Returns false if that's not possible.
  static bool Get(const std::string& path, FileStatus* status);
};

/// The contents of a file loaded by FileReader::LoadFile().
struct FileContents {
  FileContents() = default;
  FileContents(const FileContents&) = delete;
  FileContents& operator=(const FileContents&) = delete;
  ~FileContents();

  /// The contents, followed by padding NUL bytes.
  const char* data = nullptr;
  size_t size = 0;

  /// Where the contents are: in storage, or mapped.
  std::string storage;
  void* mapping = nullptr;
  size_t mapping_size = 0;

  /// The path of the file, and its status as of when it was read, if known
  /// (and it did not change while being read).
  std::string path;
  bool has_status = false;
  FileStatus status;
};

struct FileReader {
  virtual ~FileReader() {}

  /// Result of ReadFile.
  enum Status {
    Okay,
    NotFound,
    OtherError
  };

  /// Read and store in given string.  On success, return Okay.
  /// On error, return another Status and fill |err|.
  virtual Status ReadFile(const std::string& path, std::string* contents,
                          std::string* err) = 0;

  /// Whether ReadFile() may be called from several threads at once.
  virtual bool IsReadFileThreadSafe() const { return false; }

  /// Like ReadFile(), but the contents are followed by at least \a padding
  /// NUL bytes, and may be mapped into memory instead of read (for large
  /// files), so the file must not be truncated while the contents are in
  /// use.
  virtual Status LoadFile(const std::string& path, size_t padding,
                          FileContents* contents, std::string* err);
};

/// Interface for accessing the disk.
///
/// Abstract so it can be mocked out for tests.  The real implementation
/// is RealDiskInterface.
struct DiskInterface: public FileReader {
  /// stat() a file, returning the mtime, or 0 if missing and -1 on
  /// other errors.
  virtual TimeStamp Stat(const std::string& path, std::string* err) const = 0;

  /// Whether Stat() may be called from several threads at once.
  virtual bool IsStatThreadSafe() const { return false; }

  /// Create a directory, returning false on failure.
  virtual bool MakeDir(const std::string& path) = 0;

  /// Create a file, with the specified name and contents
  /// If \a crlf_on_windows is true, \n will be converted to \r\n (only on
  /// Windows builds of Ninja).
  /// Returns true on success, false on failure
  virtual bool WriteFile(const std::string& path, const std::string& contents,
                         bool crlf_on_windows) = 0;

  /// Remove the file named @a path. It behaves like 'rm -f path' so no errors
  /// are reported if it does not exists.
  /// @returns 0 if the file has been removed,
  ///          1 if the file does not exist, and
  ///          -1 if an error occurs.
  virtual int RemoveFile(const std::string& path) = 0;

  /// Create all the parent directories for path; like mkdir -p
  /// `basename path`.
  bool MakeDirs(const std::string& path);
};

/// Implementation of DiskInterface that actually hits the disk.
struct RealDiskInterface : public DiskInterface {
  RealDiskInterface();
  virtual ~RealDiskInterface();
  TimeStamp Stat(const std::string& path, std::string* err) const override;
  bool IsStatThreadSafe() const override { return true; }
  bool IsReadFileThreadSafe() const override { return true; }
  Status LoadFile(const std::string& path, size_t padding,
                  FileContents* contents, std::string* err) override;
  bool MakeDir(const std::string& path) override;
  bool WriteFile(const std::string& path, const std::string& contents,
                 bool crlf_on_windows) override;
  Status ReadFile(const std::string& path, std::string* contents,
                  std::string* err) override;
  int RemoveFile(const std::string& path) override;

  /// Whether stat information can be cached (per directory: listing one
  /// gives the information of all its files), which is only right while no
  /// command runs.  Not while Stat() is called on other threads.  Only has
  /// an effect on Windows.
  void AllowStatCache(bool allow);

  /// Whether Stat() may look up files relative to handles of their
  /// directories, which it keeps open meanwhile (raising the limit on open
  /// files if needed, until this is turned off).  That's faster for the
  /// many stat() calls of a build's start, but the handles would miss
  /// directories being replaced: only while no command runs.  Only has an
  /// effect on Linux.
  void UseDirectoryHandles(bool use);

#ifdef _WIN32
  /// Whether long paths are enabled.  Only has an effect on Windows.
  bool AreLongPathsEnabled() const;
#endif

 private:
  /// See UseDirectoryHandles().
  struct DirectoryHandles;
  std::unique_ptr<DirectoryHandles> directory_handles_;
#ifdef _WIN32
  /// Whether stat information can be cached.
  std::atomic<bool> use_cache_;

  /// Whether long paths are enabled.
  bool long_paths_enabled_;

  /// The mtimes of a directory's files, by lowercase name.
  typedef std::unordered_map<std::string, TimeStamp> DirCache;
  /// A directory's files, listed once (by the first thread that needs it).
  struct CachedDir {
    std::once_flag listed;
    bool ok = false;
    std::string err;
    DirCache files;
  };
  /// The directories, by lowercase path, in shards that are locked
  /// separately.
  struct CacheShard {
    std::mutex mutex;
    std::unordered_map<std::string, std::unique_ptr<CachedDir>> dirs;
  };
  static constexpr size_t kCacheShards = 64;
  mutable std::unique_ptr<CacheShard[]> cache_;
#endif
};

#endif  // NINJA_DISK_INTERFACE_H_
