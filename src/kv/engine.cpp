#include "kv/engine.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <vector>

#include "env/env.h"
#include "kv/block_cache.h"
#include "kv/compaction.h"
#include "kv/filename.h"
#include "kv/log_reader.h"
#include "kv/log_writer.h"
#include "kv/memtable.h"
#include "kv/table_builder.h"
#include "kv/version_set.h"
#include "util/logger.h"

namespace docstore::kv {

namespace {

// Writers stall at l0_stop_trigger until a compaction runs, so a compaction must start at or
// before that point, or the stall would never end. Same rule RocksDB applies when sanitizing.
Options SanitizeOptions(Options o) {
  o.l0_compaction_trigger = std::max(o.l0_compaction_trigger, 1);
  o.l0_slowdown_trigger = std::max(o.l0_slowdown_trigger, o.l0_compaction_trigger);
  o.l0_stop_trigger = std::max(o.l0_stop_trigger, o.l0_slowdown_trigger);
  return o;
}

}  // namespace

Engine::Engine(const Options& options, std::string dbname)
    : options_(SanitizeOptions(options)),
      env_(options.env != nullptr ? options.env : Env::Default()),
      dbname_(std::move(dbname)),
      cache_(options.block_cache_bytes > 0 ? std::make_unique<BlockCache>(options.block_cache_bytes)
                                           : nullptr),
      versions_(std::make_unique<VersionSet>(dbname_, options_, env_, cache_.get())),
      mem_(std::make_shared<MemTable>()) {}

Engine::~Engine() { (void)Close(); }

Status Engine::Open(const Options& options, const std::string& dbname,
                    std::unique_ptr<Engine>* out) {
  std::unique_ptr<Engine> engine(new Engine(options, dbname));
  Status s = engine->Recover();
  if (!s.ok()) return s;
  engine->bg_thread_ = std::thread([e = engine.get()] { e->BackgroundLoop(); });
  if (options.journal_commit_interval_ms > 0) {
    engine->syncer_thread_ = std::thread([e = engine.get()] { e->SyncerLoop(); });
  }
  *out = std::move(engine);
  return Status::OK();
}

// ---------------------------------------------------------------- shutdown

Status Engine::Close() {
  std::lock_guard<std::mutex> w(write_mutex_);  // wait for an in-flight write
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (closed_) return Status::OK();
    closed_ = true;
    shutting_down_ = true;
  }
  bg_work_cv_.notify_all();
  bg_done_cv_.notify_all();
  syncer_cv_.notify_all();
  if (bg_thread_.joinable()) bg_thread_.join();
  if (syncer_thread_.joinable()) syncer_thread_.join();
  Status s;
  if (log_) s = log_->Sync();  // make sync=false writes durable on a clean shutdown
  lock_.reset();
  return s;
}

void Engine::SyncerLoop() {
  const auto interval = std::chrono::milliseconds(options_.journal_commit_interval_ms);
  std::unique_lock<std::mutex> l(mutex_);
  while (!shutting_down_) {
    syncer_cv_.wait_for(l, interval, [&] { return shutting_down_.load(); });
    if (shutting_down_ || !bg_error_.ok()) continue;
    std::shared_ptr<LogWriter> log = log_;  // keeps the WAL open even if it is rotated away
    l.unlock();
    // Runs concurrently with writers: LogWriter::Sync only holds the WAL mutex to flush the
    // (already empty) buffer, and fdatasyncs outside it.
    Status s = log->Sync();
    stat_wal_syncs_++;
    l.lock();
    // A failed fsync cannot be retried safely (the kernel may have dropped the dirty pages),
    // so it is fatal for writes, like a failed WAL append.
    if (!s.ok() && bg_error_.ok()) bg_error_ = s;
  }
}

// ---------------------------------------------------------------- write path

Status Engine::MakeRoomForWrite(bool force) {
  std::unique_lock<std::mutex> l(mutex_);
  bool allow_delay = !force;
  while (true) {
    if (closed_) return Status::InvalidArgument("database is closed");
    if (!bg_error_.ok()) return Status::ReadOnly(bg_error_.ToString());
    const int l0_files = versions_->current()->NumFiles(0);
    if (allow_delay && l0_files >= options_.l0_slowdown_trigger) {
      // Compaction is falling behind: give it 1 ms per write instead of hitting the hard stop
      // later and stalling for a whole compaction. Only once per write.
      l.unlock();
      uint64_t start = env_->NowMicros();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      stat_stall_micros_ += env_->NowMicros() - start;
      allow_delay = false;
      l.lock();
      continue;
    }
    if (force ? mem_->num_entries() == 0
              : mem_->ApproximateMemoryUsage() < options_.memtable_size) {
      return Status::OK();
    }
    if (imm_ != nullptr || l0_files >= options_.l0_stop_trigger) {
      // The previous memtable is still being flushed, or L0 is full: wait (a write stall).
      uint64_t start = env_->NowMicros();
      bg_done_cv_.wait(l);
      stat_stall_micros_ += env_->NowMicros() - start;
      continue;
    }
    // Rotate: the full memtable becomes immutable and a new WAL starts.
    std::shared_ptr<LogWriter> old_log = log_;
    l.unlock();
    // Everything in the old WAL must be durable before any write lands in the new one;
    // otherwise power loss could keep a later (synced) write and lose an earlier one.
    Status s = old_log->Sync();
    stat_wal_syncs_++;
    uint64_t number = 0;
    std::shared_ptr<LogWriter> new_log;
    if (s.ok()) s = NewLogFile(&number, &new_log);
    l.lock();
    if (!s.ok()) {
      bg_error_ = s;
      return Status::IOError(s.message());
    }
    imm_ = std::move(mem_);
    has_imm_ = true;
    mem_ = std::make_shared<MemTable>();
    log_ = std::move(new_log);
    log_number_ = number;
    bg_work_cv_.notify_one();
    force = false;
  }
}

Status Engine::Write(const WriteOptions& options, WriteBatch* batch) {
  std::lock_guard<std::mutex> w(write_mutex_);
  Status s = MakeRoomForWrite(/*force=*/false);
  if (!s.ok()) return s;
  if (batch->Count() == 0) return Status::OK();

  batch->SetSequence(last_sequence_ + 1);
  last_sequence_ += batch->Count();
  // log_ and mem_ are only replaced by this thread (under write_mutex_), so no mutex_ needed.
  s = log_->AddRecord(batch->Contents());
  if (s.ok() && options.sync) {
    s = log_->Sync();
    stat_wal_syncs_++;
  }
  if (!s.ok()) {
    // We no longer know what the WAL contains, so stop accepting writes until reopen.
    std::lock_guard<std::mutex> l(mutex_);
    bg_error_ = s;
    return Status::IOError(s.message());
  }
  s = InsertInto(*batch, mem_.get());
  if (!s.ok()) return s;
  stat_user_bytes_ += batch->user_bytes();
  last_visible_sequence_.store(last_sequence_, std::memory_order_release);
  return Status::OK();
}

Status Engine::Put(const WriteOptions& options, std::string_view key, std::string_view value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, &batch);
}

Status Engine::Delete(const WriteOptions& options, std::string_view key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, &batch);
}

Status Engine::Flush() {
  std::lock_guard<std::mutex> w(write_mutex_);
  Status s = MakeRoomForWrite(/*force=*/true);
  if (!s.ok()) return s;
  std::unique_lock<std::mutex> l(mutex_);
  bg_done_cv_.wait(l, [&] { return imm_ == nullptr || !bg_error_.ok() || shutting_down_; });
  return bg_error_;
}

Status Engine::CompactAll() {
  Status s = Flush();
  if (!s.ok()) return s;
  std::unique_lock<std::mutex> l(mutex_);
  manual_compaction_ = true;
  bg_work_cv_.notify_one();
  bg_done_cv_.wait(l, [&] { return !manual_compaction_ || !bg_error_.ok() || shutting_down_; });
  return bg_error_;
}

// ---------------------------------------------------------------- read path

std::shared_ptr<const Snapshot> Engine::GetSnapshot() {
  std::lock_guard<std::mutex> l(mutex_);
  // One lock covers all four, so the view is consistent: every entry with sequence <= the
  // snapshot is in these memtables or in this Version's tables.
  return std::make_shared<const Snapshot>(mem_, imm_, versions_->current(),
                                          last_visible_sequence_.load(std::memory_order_acquire),
                                          &read_stats_);
}

Status Engine::Get(std::string_view key, std::string* value) {
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (closed_) return Status::InvalidArgument("database is closed");
  }
  return GetSnapshot()->Get(key, value);
}

std::unique_ptr<Iterator> Engine::NewIterator(const ReadOptions& options) {
  return GetSnapshot()->NewIterator(options);
}

Status Engine::Scan(const ReadOptions& options, size_t limit,
                    std::vector<std::pair<std::string, std::string>>* out) {
  out->clear();
  auto it = NewIterator(options);
  for (; it->Valid() && (limit == 0 || out->size() < limit); it->Next()) {
    out->emplace_back(std::string(it->key()), std::string(it->value()));
  }
  return it->status();
}

// ---------------------------------------------------------------- background work

bool Engine::NeedsCompaction() const {
  return manual_compaction_ || versions_->current()->NumFiles(0) >= options_.l0_compaction_trigger;
}

void Engine::BackgroundLoop() {
  std::unique_lock<std::mutex> l(mutex_);
  while (true) {
    bg_work_cv_.wait(l, [&] {
      return shutting_down_ || (bg_error_.ok() && (imm_ != nullptr || NeedsCompaction()));
    });
    if (shutting_down_) break;  // an unflushed imm_ is still in its WAL and is replayed on open
    // Flush first: a waiting memtable blocks writers, a pending compaction does not (yet).
    const bool flush = imm_ != nullptr;
    const bool full = manual_compaction_;
    l.unlock();
    Status s = flush ? FlushImmutable() : DoCompaction(full);
    l.lock();
    if (!flush && full) manual_compaction_ = false;
    if (!s.ok() && bg_error_.ok()) {
      Log(LogLevel::kError,
          std::string(flush ? "flush" : "compaction") + " failed: " + s.ToString());
      bg_error_ = s;
    }
    bg_done_cv_.notify_all();
  }
}

Status Engine::DoCompaction(bool full) {
  const uint64_t start = env_->NowMicros();
  Compaction c;
  {
    std::lock_guard<std::mutex> l(mutex_);
    c = PickCompaction(*versions_->current(), full);
  }
  if (c.empty()) return Status::OK();
  // Called every few hundred entries. A long compaction must not starve writers waiting for a
  // flush, so a pending memtable is flushed right here (into L0, which is not an input).
  auto between_entries = [this] {
    if (shutting_down_) return false;
    if (has_imm_) {
      Status s = FlushImmutable();
      std::lock_guard<std::mutex> l(mutex_);
      if (!s.ok() && bg_error_.ok()) bg_error_ = s;
      bg_done_cv_.notify_all();
      return s.ok();
    }
    return true;
  };
  CompactionResult result;
  bool aborted = false;
  Status s = RunCompaction(options_, env_, dbname_, versions_.get(), c, between_entries, &result,
                           &aborted);
  if (!s.ok() || aborted) return s;
  Hook("compaction:before_install");
  VersionEdit edit;
  for (int level = 0; level < kNumLevels; level++) {
    for (const auto& f : c.inputs[level]) edit.DeleteFile(level, f->number);
  }
  for (auto& meta : result.outputs) edit.AddFile(1, meta);
  s = versions_->LogAndApply(&edit, last_visible_sequence_.load(std::memory_order_acquire),
                             &mutex_);
  if (!s.ok()) {
    for (auto& meta : result.outputs) meta.table->MarkObsolete();  // never installed
    return s;
  }
  Hook("compaction:after_install");
  stat_compactions_++;
  stat_compaction_bytes_read_ += c.InputBytes();
  stat_compaction_bytes_written_ += result.bytes_written;
  stat_compaction_micros_ += env_->NowMicros() - start;
  return Status::OK();
}

Status Engine::FlushImmutable() {
  Hook("flush:begin");
  const uint64_t start = env_->NowMicros();
  std::shared_ptr<MemTable> imm;
  uint64_t log_number;
  {
    std::lock_guard<std::mutex> l(mutex_);
    imm = imm_;
    log_number = log_number_;
  }
  FileMeta meta;
  Status s = WriteLevel0Table(*imm, &meta);
  if (!s.ok()) return s;
  Hook("flush:after_table");
  VersionEdit edit;
  if (meta.table) edit.AddFile(0, std::move(meta));
  // Every write in imm_ is in WALs older than log_number, which this edit retires.
  edit.SetLogNumber(log_number);
  s = versions_->LogAndApply(&edit, last_visible_sequence_.load(std::memory_order_acquire),
                             &mutex_);
  if (!s.ok()) return s;
  Hook("flush:after_manifest");
  {
    std::lock_guard<std::mutex> l(mutex_);
    imm_.reset();
    has_imm_ = false;
  }
  stat_flushes_++;
  stat_flush_micros_ += env_->NowMicros() - start;
  DeleteObsoleteFiles(/*at_open=*/false);
  return Status::OK();
}

Status Engine::WriteLevel0Table(const MemTable& mem, FileMeta* meta) {
  if (mem.num_entries() == 0) return Status::OK();
  meta->number = versions_->NewFileNumber();
  const std::string path = TableFileName(dbname_, meta->number);
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(path, &file);
  if (!s.ok()) return s;
  TableBuilder builder(options_, std::move(file));
  auto it = mem.NewIterator();
  std::string last_user_key;
  bool has_last = false;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string_view user_key = ExtractUserKey(it->key());
    // Versions of a key are adjacent, newest first. A flushed table is only read by snapshots
    // taken after it is installed, which are newer than all its entries, so older versions
    // are dead (tombstones are kept: they shadow older tables).
    if (has_last && user_key == last_user_key) continue;
    last_user_key.assign(user_key);
    has_last = true;
    builder.Add(it->key(), it->value());
  }
  s = builder.Finish();
  // The MANIFEST will name this file, so its directory entry must be durable first.
  if (s.ok()) s = env_->SyncDir(dbname_);
  if (s.ok()) {
    meta->file_size = builder.FileSize();
    meta->smallest = builder.smallest_key();
    meta->largest = builder.largest_key();
    s = versions_->OpenTable(meta);
  }
  if (!s.ok()) {
    (void)env_->DeleteFile(path);
    return s;
  }
  stat_flush_bytes_ += meta->file_size;
  return Status::OK();
}

// ---------------------------------------------------------------- stats

nlohmann::json Engine::Stats() {
  std::shared_ptr<const Version> version;
  nlohmann::json j;
  {
    std::lock_guard<std::mutex> l(mutex_);
    version = versions_->current();
    j["memtable_bytes"] = mem_->ApproximateMemoryUsage();
    j["has_immutable_memtable"] = imm_ != nullptr;
    j["bg_error"] = bg_error_.ok() ? "" : bg_error_.ToString();
  }
  j["l0_files"] = version->NumFiles(0);
  j["l0_bytes"] = version->LevelBytes(0);
  j["l1_files"] = version->NumFiles(1);
  j["l1_bytes"] = version->LevelBytes(1);
  j["last_sequence"] = last_visible_sequence_.load();
  const uint64_t user = stat_user_bytes_.load();
  const uint64_t flushed = stat_flush_bytes_.load();
  const uint64_t compacted = stat_compaction_bytes_written_.load();
  j["flushes"] = stat_flushes_.load();
  j["flush_bytes_written"] = flushed;
  j["flush_micros"] = stat_flush_micros_.load();
  j["compactions"] = stat_compactions_.load();
  j["compaction_bytes_read"] = stat_compaction_bytes_read_.load();
  j["compaction_bytes_written"] = compacted;
  j["compaction_micros"] = stat_compaction_micros_.load();
  j["user_bytes_written"] = user;
  // SST bytes written by flush + compaction per byte of user key/value data.
  j["write_amplification"] = user == 0 ? 0.0 : static_cast<double>(flushed + compacted) / user;
  j["bloom_checks"] = read_stats_.bloom_checks.load();
  j["bloom_useful"] = read_stats_.bloom_useful.load();
  j["stall_micros"] = stat_stall_micros_.load();
  j["wal_syncs"] = stat_wal_syncs_.load();
  j["block_cache_hits"] = cache_ ? cache_->hits() : 0;
  j["block_cache_misses"] = cache_ ? cache_->misses() : 0;
  j["block_cache_evictions"] = cache_ ? cache_->evictions() : 0;
  return j;
}

}  // namespace docstore::kv
