// Env backed by POSIX system calls. Every descriptor is opened O_CLOEXEC so a forked child
// (the crash-test driver execs crash_writer) never inherits database files.

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>

#include "env/env.h"

namespace docstore {
namespace {

namespace fs = std::filesystem;

Status ErrnoStatus(const std::string& context, int err) {
  std::string msg = context + ": " + std::strerror(err);
  return err == ENOENT ? Status::NotFound(msg) : Status::IOError(msg);
}

Status FsStatus(const std::string& context, const std::error_code& ec) {
  std::string msg = context + ": " + ec.message();
  return ec == std::errc::no_such_file_or_directory ? Status::NotFound(msg) : Status::IOError(msg);
}

class PosixWritableFile final : public WritableFile {
 public:
  static constexpr size_t kBufferSize = 64 * 1024;

  PosixWritableFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {
    buffer_.reserve(kBufferSize);
  }
  ~PosixWritableFile() override {
    if (fd_ >= 0) (void)Close();
  }

  Status Append(std::string_view data) override {
    if (buffer_.size() + data.size() <= kBufferSize) {
      buffer_.append(data);
      return Status::OK();
    }
    Status s = Flush();
    if (!s.ok()) return s;
    if (data.size() >= kBufferSize) return WriteAll(data);  // too big to be worth copying
    buffer_.append(data);
    return Status::OK();
  }

  Status Flush() override {
    Status s = WriteAll(buffer_);
    buffer_.clear();
    return s;
  }

  Status Sync() override {
    // fdatasync skips metadata such as mtime but still persists the file size, which is
    // all that appends need.
    if (::fdatasync(fd_) != 0) return ErrnoStatus(path_, errno);
    return Status::OK();
  }

  Status Close() override {
    Status s = Flush();
    if (::close(fd_) != 0 && s.ok()) s = ErrnoStatus(path_, errno);
    fd_ = -1;
    return s;
  }

 private:
  Status WriteAll(std::string_view data) {
    while (!data.empty()) {
      ssize_t n = ::write(fd_, data.data(), data.size());
      if (n < 0) {
        if (errno == EINTR) continue;
        return ErrnoStatus(path_, errno);
      }
      data.remove_prefix(static_cast<size_t>(n));
    }
    return Status::OK();
  }

  std::string path_;
  int fd_;
  std::string buffer_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  ~PosixRandomAccessFile() override { ::close(fd_); }

  Status Read(uint64_t offset, size_t n, std::string* out) const override {
    out->resize(n);
    size_t done = 0;
    while (done < n) {
      ssize_t r = ::pread(fd_, out->data() + done, n - done, static_cast<off_t>(offset + done));
      if (r < 0) {
        if (errno == EINTR) continue;
        return ErrnoStatus(path_, errno);
      }
      if (r == 0) break;  // end of file
      done += static_cast<size_t>(r);
    }
    out->resize(done);
    return Status::OK();
  }

 private:
  std::string path_;
  int fd_;
};

class PosixSequentialFile final : public SequentialFile {
 public:
  PosixSequentialFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  ~PosixSequentialFile() override { ::close(fd_); }

  Status Read(size_t n, std::string* out) override {
    out->resize(n);
    size_t done = 0;
    while (done < n) {
      ssize_t r = ::read(fd_, out->data() + done, n - done);
      if (r < 0) {
        if (errno == EINTR) continue;
        return ErrnoStatus(path_, errno);
      }
      if (r == 0) break;
      done += static_cast<size_t>(r);
    }
    out->resize(done);
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) < 0) return ErrnoStatus(path_, errno);
    return Status::OK();
  }

 private:
  std::string path_;
  int fd_;
};

class PosixFileLock final : public FileLock {
 public:
  explicit PosixFileLock(int fd) : fd_(fd) {}
  ~PosixFileLock() override {
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
  }

 private:
  int fd_;
};

class PosixEnv final : public Env {
 public:
  Status NewWritableFile(const std::string& path, std::unique_ptr<WritableFile>* out) override {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return ErrnoStatus(path, errno);
    *out = std::make_unique<PosixWritableFile>(path, fd);
    return Status::OK();
  }

  Status NewRandomAccessFile(const std::string& path,
                             std::unique_ptr<RandomAccessFile>* out) override {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return ErrnoStatus(path, errno);
    *out = std::make_unique<PosixRandomAccessFile>(path, fd);
    return Status::OK();
  }

  Status NewSequentialFile(const std::string& path, std::unique_ptr<SequentialFile>* out) override {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return ErrnoStatus(path, errno);
    *out = std::make_unique<PosixSequentialFile>(path, fd);
    return Status::OK();
  }

  bool FileExists(const std::string& path) override { return ::access(path.c_str(), F_OK) == 0; }

  Status GetChildren(const std::string& dir, std::vector<std::string>* names) override {
    names->clear();
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      names->push_back(it->path().filename().string());
    }
    return ec ? FsStatus(dir, ec) : Status::OK();
  }

  Status GetFileSize(const std::string& path, uint64_t* size) override {
    std::error_code ec;
    *size = fs::file_size(path, ec);
    return ec ? FsStatus(path, ec) : Status::OK();
  }

  Status DeleteFile(const std::string& path) override {
    if (::unlink(path.c_str()) != 0) return ErrnoStatus(path, errno);
    return Status::OK();
  }

  Status RenameFile(const std::string& from, const std::string& to) override {
    if (::rename(from.c_str(), to.c_str()) != 0) return ErrnoStatus(from, errno);
    return Status::OK();
  }

  Status CreateDir(const std::string& path) override {
    std::error_code ec;
    fs::create_directories(path, ec);
    return ec ? FsStatus(path, ec) : Status::OK();
  }

  Status SyncDir(const std::string& path) override {
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return ErrnoStatus(path, errno);
    Status s;
    if (::fsync(fd) != 0) s = ErrnoStatus(path, errno);
    ::close(fd);
    return s;
  }

  Status LockFile(const std::string& path, std::unique_ptr<FileLock>* lock) override {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return ErrnoStatus(path, errno);
    // flock locks belong to the open file description, so a second open of the same database
    // fails even from the same process.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      int err = errno;
      ::close(fd);
      return Status::IOError(path + ": database is locked by another user (" + std::strerror(err) +
                             ")");
    }
    *lock = std::make_unique<PosixFileLock>(fd);
    return Status::OK();
  }

  uint64_t NowMicros() override {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};

}  // namespace

Env* Env::Default() {
  static PosixEnv env;
  return &env;
}

}  // namespace docstore
