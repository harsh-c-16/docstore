#pragma once

// The storage engine: sorted byte keys -> byte values, crash-safe through a write-ahead log,
// with a two-level LSM tree of SSTables. It knows nothing about documents (see src/doc).
//
// Threads: callers (any number of readers, writers serialized by write_mutex_) and one
// background thread that flushes memtables (and compacts). Lock order is documented in
// docs/DESIGN.md: write_mutex_ -> mutex_ -> WAL mutex -> cache shard mutexes.

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#include "docstore/options.h"
#include "docstore/status.h"
#include "kv/dbformat.h"
#include "kv/version_edit.h"
#include "kv/write_batch.h"

namespace docstore {
class Env;
class FileLock;
}  // namespace docstore

namespace docstore::kv {

class BlockCache;
class LogWriter;
class MemTable;
class VersionSet;
struct Version;

class Engine {
 public:
  static Status Open(const Options& options, const std::string& dbname,
                     std::unique_ptr<Engine>* out);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Clean shutdown: stops the background thread, syncs the WAL, releases the LOCK.
  Status Close();

  // Applies the batch atomically: after a crash either every record is recovered or none.
  Status Write(const WriteOptions& options, WriteBatch* batch);
  Status Put(const WriteOptions& options, std::string_view key, std::string_view value);
  Status Delete(const WriteOptions& options, std::string_view key);

  // NotFound if the key is absent or deleted.
  Status Get(std::string_view key, std::string* value);

  // Forces the memtable into an L0 SSTable and waits for it.
  Status Flush();

  nlohmann::json Stats();

 private:
  Engine(const Options& options, std::string dbname);

  Status Recover();
  Status ReplayLog(uint64_t number, bool newest, std::shared_ptr<MemTable>* mem,
                   VersionEdit* edit, SequenceNumber* max_sequence);
  Status NewLogFile(uint64_t* number, std::shared_ptr<LogWriter>* log);
  void DeleteObsoleteFiles(bool at_open);

  // Write path (write_mutex_ held). force=true rotates a non-empty memtable even if not full.
  Status MakeRoomForWrite(bool force);

  // Background thread.
  void BackgroundLoop();
  Status FlushImmutable();
  // Writes mem's newest entry per user key to a new L0 table. Leaves meta->table null if the
  // memtable is empty.
  Status WriteLevel0Table(const MemTable& mem, FileMeta* meta);

  void Hook(std::string_view point) {
    if (options_.test_hook) options_.test_hook(point);
  }

  const Options options_;
  Env* const env_;
  const std::string dbname_;
  std::unique_ptr<FileLock> lock_;
  std::unique_ptr<BlockCache> cache_;  // declared before versions_: tables point into it
  std::unique_ptr<VersionSet> versions_;

  // Serializes writers. Readers never take it.
  std::mutex write_mutex_;
  SequenceNumber last_sequence_ = 0;  // last sequence assigned; guarded by write_mutex_

  // Protects everything below. Held only for pointer swaps and flags, never during I/O.
  mutable std::mutex mutex_;
  std::condition_variable bg_work_cv_;  // wakes the background thread
  std::condition_variable bg_done_cv_;  // background work finished (stalled writers, Flush)
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;  // memtable being flushed, or null
  std::shared_ptr<LogWriter> log_;
  uint64_t log_number_ = 0;  // WAL that receives writes after imm_ (the flush's log_number)
  Status bg_error_;
  bool shutting_down_ = false;
  bool closed_ = false;
  std::thread bg_thread_;

  // Highest sequence whose batch is fully in the memtable. Readers snapshot it (acquire);
  // the writer publishes a batch by storing it (release) after the memtable insert, so a
  // reader sees either all of a batch or none of it.
  std::atomic<SequenceNumber> last_visible_sequence_{0};

  // Statistics.
  std::atomic<uint64_t> stat_flushes_{0};
  std::atomic<uint64_t> stat_flush_bytes_{0};
  std::atomic<uint64_t> stat_bloom_checks_{0};
  std::atomic<uint64_t> stat_bloom_useful_{0};
  std::atomic<uint64_t> stat_stall_micros_{0};
  std::atomic<uint64_t> stat_user_bytes_{0};
};

}  // namespace docstore::kv
