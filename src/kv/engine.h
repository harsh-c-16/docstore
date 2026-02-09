#pragma once

// The storage engine: sorted byte keys -> byte values, crash-safe through a write-ahead log.
// It knows nothing about documents (see src/doc for that layer).

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "docstore/options.h"
#include "docstore/status.h"
#include "kv/dbformat.h"
#include "kv/write_batch.h"

namespace docstore {
class Env;
class FileLock;
}  // namespace docstore

namespace docstore::kv {

class LogWriter;
class MemTable;

class Engine {
 public:
  static Status Open(const Options& options, const std::string& dbname,
                     std::unique_ptr<Engine>* out);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Clean shutdown: syncs the WAL and releases the LOCK. Safe to call twice.
  Status Close();

  // Applies the batch atomically: after a crash either every record is recovered or none.
  Status Write(const WriteOptions& options, WriteBatch* batch);
  Status Put(const WriteOptions& options, std::string_view key, std::string_view value);
  Status Delete(const WriteOptions& options, std::string_view key);

  // NotFound if the key is absent or deleted.
  Status Get(std::string_view key, std::string* value);

 private:
  Engine(const Options& options, std::string dbname);

  Status Recover();
  Status ReplayLog(uint64_t number, bool newest);
  Status CreateNewLog();

  const Options options_;
  Env* const env_;
  const std::string dbname_;
  std::unique_ptr<FileLock> lock_;

  // Serializes writers (DESIGN.md, lock order). Readers never take it.
  std::mutex write_mutex_;
  SequenceNumber last_sequence_ = 0;  // last sequence assigned; guarded by write_mutex_
  uint64_t next_file_number_ = 1;     // guarded by write_mutex_
  std::shared_ptr<LogWriter> log_;
  uint64_t log_number_ = 0;

  // Protects the pointers below. Held only for pointer copies, never during I/O.
  mutable std::mutex mutex_;
  std::shared_ptr<MemTable> mem_;
  Status bg_error_;
  bool closed_ = false;

  // Highest sequence whose batch is fully in the memtable. Readers snapshot it (acquire);
  // the writer publishes a batch by storing it (release) after the memtable insert, so a
  // reader sees either all of a batch or none of it.
  std::atomic<SequenceNumber> last_visible_sequence_{0};
};

}  // namespace docstore::kv
