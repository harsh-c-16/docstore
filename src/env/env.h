#pragma once

// Filesystem abstraction. The engine only touches files through Env, so tests can swap in an
// in-memory filesystem (MemEnv) or one that simulates power loss (FaultInjectionEnv).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "docstore/status.h"

namespace docstore {

// Append-only file. Not thread-safe; callers serialize access (the WAL uses its own mutex).
class WritableFile {
 public:
  virtual ~WritableFile() = default;
  virtual Status Append(std::string_view data) = 0;
  // Hands buffered bytes to the OS. After Flush, the data survives a *process* crash.
  virtual Status Flush() = 0;
  // Makes everything already Flush()ed durable (fdatasync), so it survives *power loss*.
  // Deliberately does not touch the user-space buffer: the WAL syncer calls Sync from another
  // thread while writers keep appending (DESIGN.md D6).
  virtual Status Sync() = 0;
  virtual Status Close() = 0;
};

// Positional reads. Thread-safe. A read past end-of-file returns fewer bytes, not an error.
class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;
  virtual Status Read(uint64_t offset, size_t n, std::string* out) const = 0;
};

class SequentialFile {
 public:
  virtual ~SequentialFile() = default;
  // Reads up to n bytes; fewer means end-of-file.
  virtual Status Read(size_t n, std::string* out) = 0;
  virtual Status Skip(uint64_t n) = 0;
};

// Held for as long as the database is open. Destroying it releases the lock.
class FileLock {
 public:
  virtual ~FileLock() = default;
};

class Env {
 public:
  virtual ~Env() = default;

  // The process-wide PosixEnv.
  static Env* Default();

  // Creates the file, or truncates it if it exists.
  virtual Status NewWritableFile(const std::string& path, std::unique_ptr<WritableFile>* out) = 0;
  virtual Status NewRandomAccessFile(const std::string& path,
                                     std::unique_ptr<RandomAccessFile>* out) = 0;
  virtual Status NewSequentialFile(const std::string& path,
                                   std::unique_ptr<SequentialFile>* out) = 0;

  virtual bool FileExists(const std::string& path) = 0;
  // File names (not paths) directly inside dir.
  virtual Status GetChildren(const std::string& dir, std::vector<std::string>* names) = 0;
  virtual Status GetFileSize(const std::string& path, uint64_t* size) = 0;
  virtual Status DeleteFile(const std::string& path) = 0;
  // Atomically replaces `to` if it exists.
  virtual Status RenameFile(const std::string& from, const std::string& to) = 0;
  // Succeeds if the directory already exists.
  virtual Status CreateDir(const std::string& path) = 0;
  // Makes creates, renames and deletes inside dir durable.
  virtual Status SyncDir(const std::string& path) = 0;

  // Fails with IOError if another holder (process or Env user) has the lock.
  virtual Status LockFile(const std::string& path, std::unique_ptr<FileLock>* lock) = 0;
  Status UnlockFile(std::unique_ptr<FileLock> lock) {
    lock.reset();
    return Status::OK();
  }

  // Monotonic clock for durations.
  virtual uint64_t NowMicros() = 0;
};

Status ReadFileToString(Env* env, const std::string& path, std::string* out);
// Write + Flush + Sync + Close: the file's contents are durable on return (its name is not;
// that needs SyncDir on the parent directory).
Status WriteStringToFileSync(Env* env, std::string_view data, const std::string& path);

}  // namespace docstore
