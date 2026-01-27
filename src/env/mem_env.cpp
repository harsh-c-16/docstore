#include "env/mem_env.h"

#include <chrono>

namespace docstore {
namespace {

using FileState = MemEnv::FileState;

class MemWritableFile final : public WritableFile {
 public:
  MemWritableFile(MemEnv* env, std::shared_ptr<FileState> state)
      : env_(env), state_(std::move(state)) {}

  Status Append(std::string_view data) override {
    std::lock_guard<std::mutex> l(state_->mu);
    Status s = env_->OnAppend();
    if (s.ok()) state_->data.append(data);
    return s;
  }
  Status Flush() override { return Status::OK(); }  // no user-space buffer
  Status Sync() override {
    std::lock_guard<std::mutex> l(state_->mu);
    Status s = env_->OnSync();
    if (s.ok()) state_->synced_size = state_->data.size();
    return s;
  }
  Status Close() override { return Status::OK(); }

 private:
  MemEnv* env_;
  std::shared_ptr<FileState> state_;
};

class MemRandomAccessFile final : public RandomAccessFile {
 public:
  explicit MemRandomAccessFile(std::shared_ptr<FileState> state) : state_(std::move(state)) {}

  Status Read(uint64_t offset, size_t n, std::string* out) const override {
    std::lock_guard<std::mutex> l(state_->mu);
    if (offset >= state_->data.size()) {
      out->clear();
    } else {
      out->assign(state_->data, offset, n);
    }
    return Status::OK();
  }

 private:
  std::shared_ptr<FileState> state_;
};

class MemSequentialFile final : public SequentialFile {
 public:
  explicit MemSequentialFile(std::shared_ptr<FileState> state) : state_(std::move(state)) {}

  Status Read(size_t n, std::string* out) override {
    std::lock_guard<std::mutex> l(state_->mu);
    if (pos_ >= state_->data.size()) {
      out->clear();
    } else {
      out->assign(state_->data, pos_, n);
    }
    pos_ += out->size();
    return Status::OK();
  }
  Status Skip(uint64_t n) override {
    pos_ += n;
    return Status::OK();
  }

 private:
  std::shared_ptr<FileState> state_;
  size_t pos_ = 0;
};

class MemFileLock final : public FileLock {
 public:
  MemFileLock(MemEnv* env, std::string path) : env_(env), path_(std::move(path)) {}
  ~MemFileLock() override { env_->ReleaseLock(path_); }

 private:
  MemEnv* env_;
  std::string path_;
};

}  // namespace

std::string MemEnv::Parent(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

Status MemEnv::NewWritableFile(const std::string& path, std::unique_ptr<WritableFile>* out) {
  std::lock_guard<std::mutex> l(mu_);
  Status s = OnMetadataChange();
  if (!s.ok()) return s;
  if (!dirs_.count(Parent(path))) return Status::NotFound(path + ": parent directory missing");
  // A fresh inode, as with O_TRUNC on a real filesystem: old readers keep the old contents.
  auto state = std::make_shared<FileState>();
  files_[path] = state;
  *out = std::make_unique<MemWritableFile>(this, std::move(state));
  return Status::OK();
}

Status MemEnv::NewRandomAccessFile(const std::string& path,
                                   std::unique_ptr<RandomAccessFile>* out) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = files_.find(path);
  if (it == files_.end()) return Status::NotFound(path);
  *out = std::make_unique<MemRandomAccessFile>(it->second);
  return Status::OK();
}

Status MemEnv::NewSequentialFile(const std::string& path, std::unique_ptr<SequentialFile>* out) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = files_.find(path);
  if (it == files_.end()) return Status::NotFound(path);
  *out = std::make_unique<MemSequentialFile>(it->second);
  return Status::OK();
}

bool MemEnv::FileExists(const std::string& path) {
  std::lock_guard<std::mutex> l(mu_);
  return files_.count(path) || dirs_.count(path);
}

Status MemEnv::GetChildren(const std::string& dir, std::vector<std::string>* names) {
  std::lock_guard<std::mutex> l(mu_);
  names->clear();
  for (const auto& [path, state] : files_) {
    if (Parent(path) == dir) names->push_back(path.substr(dir.size() + 1));
  }
  return Status::OK();
}

Status MemEnv::GetFileSize(const std::string& path, uint64_t* size) {
  std::shared_ptr<FileState> state;
  {
    std::lock_guard<std::mutex> l(mu_);
    auto it = files_.find(path);
    if (it == files_.end()) return Status::NotFound(path);
    state = it->second;
  }
  std::lock_guard<std::mutex> l(state->mu);
  *size = state->data.size();
  return Status::OK();
}

Status MemEnv::DeleteFile(const std::string& path) {
  std::lock_guard<std::mutex> l(mu_);
  Status s = OnMetadataChange();
  if (!s.ok()) return s;
  if (files_.erase(path) == 0) return Status::NotFound(path);
  return Status::OK();
}

Status MemEnv::RenameFile(const std::string& from, const std::string& to) {
  std::lock_guard<std::mutex> l(mu_);
  Status s = OnMetadataChange();
  if (!s.ok()) return s;
  auto it = files_.find(from);
  if (it == files_.end()) return Status::NotFound(from);
  auto state = it->second;
  files_.erase(it);
  files_[to] = std::move(state);
  return Status::OK();
}

Status MemEnv::CreateDir(const std::string& path) {
  // Directories are treated as durable as soon as they exist (DESIGN.md D5).
  std::lock_guard<std::mutex> l(mu_);
  dirs_.insert(path);
  return Status::OK();
}

Status MemEnv::SyncDir(const std::string& path) {
  std::lock_guard<std::mutex> l(mu_);
  return OnSyncDir(path);
}

Status MemEnv::LockFile(const std::string& path, std::unique_ptr<FileLock>* lock) {
  std::lock_guard<std::mutex> l(mu_);
  if (!locks_.insert(path).second) {
    return Status::IOError(path + ": database is locked by another user");
  }
  *lock = std::make_unique<MemFileLock>(this, path);
  return Status::OK();
}

void MemEnv::ReleaseLock(const std::string& path) {
  std::lock_guard<std::mutex> l(mu_);
  locks_.erase(path);
}

uint64_t MemEnv::NowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace docstore
