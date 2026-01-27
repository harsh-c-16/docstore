#pragma once

// In-memory filesystem for fast, deterministic tests. It also tracks how much of each file has
// been Sync()ed, which FaultInjectionEnv uses to model power loss.

#include <map>
#include <mutex>
#include <set>

#include "env/env.h"

namespace docstore {

class MemEnv : public Env {
 public:
  MemEnv() = default;
  ~MemEnv() override = default;

  Status NewWritableFile(const std::string& path, std::unique_ptr<WritableFile>* out) override;
  Status NewRandomAccessFile(const std::string& path,
                             std::unique_ptr<RandomAccessFile>* out) override;
  Status NewSequentialFile(const std::string& path, std::unique_ptr<SequentialFile>* out) override;
  bool FileExists(const std::string& path) override;
  Status GetChildren(const std::string& dir, std::vector<std::string>* names) override;
  Status GetFileSize(const std::string& path, uint64_t* size) override;
  Status DeleteFile(const std::string& path) override;
  Status RenameFile(const std::string& from, const std::string& to) override;
  Status CreateDir(const std::string& path) override;
  Status SyncDir(const std::string& path) override;
  Status LockFile(const std::string& path, std::unique_ptr<FileLock>* lock) override;
  uint64_t NowMicros() override;

  // One "inode". Files are append-only after creation, so the durable contents of a file are
  // exactly data[0, synced_size).
  struct FileState {
    std::mutex mu;
    std::string data;
    size_t synced_size = 0;
  };

  // Fault hooks, overridden by FaultInjectionEnv. OnAppend and OnSync run while the file's
  // mutex is held; OnMetadataChange and OnSyncDir run while mu_ is held.
  virtual Status OnAppend() { return Status::OK(); }
  virtual Status OnSync() { return Status::OK(); }
  virtual Status OnMetadataChange() { return Status::OK(); }
  virtual Status OnSyncDir(const std::string& /*dir*/) { return Status::OK(); }

  void ReleaseLock(const std::string& path);

 protected:
  static std::string Parent(const std::string& path);

  std::mutex mu_;
  std::map<std::string, std::shared_ptr<FileState>> files_;  // the visible namespace
  std::set<std::string> dirs_;
  std::set<std::string> locks_;
};

}  // namespace docstore
