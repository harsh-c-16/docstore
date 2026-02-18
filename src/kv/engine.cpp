#include "kv/engine.h"

#include <algorithm>
#include <set>
#include <vector>

#include "env/env.h"
#include "kv/block_cache.h"
#include "kv/filename.h"
#include "kv/log_reader.h"
#include "kv/log_writer.h"
#include "kv/memtable.h"
#include "kv/table_builder.h"
#include "kv/version_set.h"
#include "util/logger.h"

namespace docstore::kv {

Engine::Engine(const Options& options, std::string dbname)
    : options_(options),
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
  *out = std::move(engine);
  return Status::OK();
}

// ---------------------------------------------------------------- recovery

Status Engine::Recover() {
  Status s;
  if (options_.create_if_missing) s = env_->CreateDir(dbname_);
  if (s.ok()) s = env_->LockFile(LockFileName(dbname_), &lock_);
  if (!s.ok()) return s;

  if (env_->FileExists(CurrentFileName(dbname_))) {
    if (options_.error_if_exists) return Status::InvalidArgument(dbname_ + " already exists");
    s = versions_->Recover();
    if (!s.ok()) return s;
  } else if (!options_.create_if_missing) {
    return Status::InvalidArgument(dbname_ + " does not exist (create_if_missing is false)");
  }

  // Replay every WAL that is not yet covered by an SSTable, oldest first.
  std::vector<std::string> children;
  s = env_->GetChildren(dbname_, &children);
  if (!s.ok()) return s;
  std::vector<uint64_t> logs;
  for (const auto& name : children) {
    uint64_t number = 0;
    FileType type = ParseFileName(name, &number);
    if (type == FileType::kLog || type == FileType::kTable || type == FileType::kManifest) {
      versions_->MarkFileNumberUsed(number);  // never reuse a number that is on disk
    }
    if (type == FileType::kLog && number >= versions_->log_number()) logs.push_back(number);
  }
  std::sort(logs.begin(), logs.end());

  VersionEdit edit;
  auto mem = std::make_shared<MemTable>();
  SequenceNumber max_sequence = versions_->last_sequence();
  for (size_t i = 0; i < logs.size(); i++) {
    s = ReplayLog(logs[i], i + 1 == logs.size(), &mem, &edit, &max_sequence);
    if (!s.ok()) return s;
  }
  // Recovered data goes to L0 now, so the old WALs can be retired after the snapshot below.
  FileMeta meta;
  s = WriteLevel0Table(*mem, &meta);
  if (!s.ok()) return s;
  if (meta.table) edit.AddFile(0, std::move(meta));
  versions_->ApplyWithoutLogging(edit);

  last_sequence_ = max_sequence;
  last_visible_sequence_.store(max_sequence, std::memory_order_release);
  s = NewLogFile(&log_number_, &log_);
  if (s.ok()) s = versions_->WriteSnapshot(log_number_, max_sequence);
  if (!s.ok()) return s;
  Hook("open:after_snapshot");
  // Only now, with CURRENT naming a manifest that covers all replayed data, is it safe to
  // delete the old WALs and manifest.
  DeleteObsoleteFiles(/*at_open=*/true);
  return Status::OK();
}

Status Engine::ReplayLog(uint64_t number, bool newest, std::shared_ptr<MemTable>* mem,
                         VersionEdit* edit, SequenceNumber* max_sequence) {
  const std::string fname = LogFileName(dbname_, number);
  std::string contents;
  Status s = ReadFileToString(env_, fname, &contents);
  if (!s.ok()) return s;
  LogReader reader(contents);
  std::string_view record;
  WriteBatch batch;
  while (reader.ReadRecord(&record)) {
    s = batch.SetContents(record);
    if (s.ok()) s = InsertInto(batch, mem->get());
    if (!s.ok()) return Status::Corruption(fname + ": " + s.message());
    if (batch.Count() > 0) {
      *max_sequence = std::max(*max_sequence, batch.Sequence() + batch.Count() - 1);
    }
    if ((*mem)->ApproximateMemoryUsage() >= options_.memtable_size) {
      FileMeta meta;
      s = WriteLevel0Table(**mem, &meta);
      if (!s.ok()) return s;
      edit->AddFile(0, std::move(meta));
      *mem = std::make_shared<MemTable>();
    }
  }
  if (reader.problem() == LogReader::Problem::kNone) return Status::OK();
  if (newest) {
    // A crash in the middle of an append leaves a torn record at the end of the newest WAL.
    // That write was never acknowledged, so recovery stops at the last good record. The file
    // is retired at the end of Open, so it is not truncated in place (DESIGN.md X1).
    Log(LogLevel::kWarn, fname + ": ignoring torn tail after byte " +
                             std::to_string(reader.valid_prefix_length()) + " (" +
                             reader.status().message() + ")");
    return Status::OK();
  }
  if (options_.paranoid_checks) return Status::Corruption(fname + ": " + reader.status().message());
  Log(LogLevel::kError, fname + ": skipping rest of file: " + reader.status().message());
  return Status::OK();
}

Status Engine::NewLogFile(uint64_t* number, std::shared_ptr<LogWriter>* log) {
  uint64_t n = versions_->NewFileNumber();
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(LogFileName(dbname_, n), &file);
  // The new file's directory entry must be durable before any sync=true write in it is
  // acknowledged, otherwise power loss could take the whole file with it.
  if (s.ok()) s = env_->SyncDir(dbname_);
  if (!s.ok()) return s;
  *number = n;
  *log = std::make_shared<LogWriter>(std::move(file));
  return Status::OK();
}

void Engine::DeleteObsoleteFiles(bool at_open) {
  std::set<uint64_t> live;
  uint64_t min_log;
  {
    std::lock_guard<std::mutex> l(mutex_);
    for (const auto& files : versions_->current()->files) {
      for (const auto& f : files) live.insert(f->number);
    }
    min_log = versions_->log_number();
  }
  std::vector<std::string> children;
  if (!env_->GetChildren(dbname_, &children).ok()) return;
  for (const auto& name : children) {
    uint64_t number = 0;
    bool remove = false;
    switch (ParseFileName(name, &number)) {
      case FileType::kLog:
        remove = number < min_log;
        break;
      // Tables are normally deleted by ~TableFile; at open, any table not in the version is
      // an orphan of an interrupted flush or compaction.
      case FileType::kTable:
        remove = at_open && !live.count(number);
        break;
      case FileType::kManifest:
        remove = at_open && number != versions_->manifest_number();
        break;
      case FileType::kTemp:
        remove = at_open;
        break;
      default:
        break;
    }
    if (remove) (void)env_->DeleteFile(dbname_ + "/" + name);
  }
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
  if (bg_thread_.joinable()) bg_thread_.join();
  Status s;
  if (log_) s = log_->Sync();  // make sync=false writes durable on a clean shutdown
  lock_.reset();
  return s;
}

// ---------------------------------------------------------------- write path

Status Engine::MakeRoomForWrite(bool force) {
  std::unique_lock<std::mutex> l(mutex_);
  while (true) {
    if (closed_) return Status::InvalidArgument("database is closed");
    if (!bg_error_.ok()) return Status::ReadOnly(bg_error_.ToString());
    if (force ? mem_->num_entries() == 0
              : mem_->ApproximateMemoryUsage() < options_.memtable_size) {
      return Status::OK();
    }
    if (imm_ != nullptr) {
      // The previous memtable is still being flushed: wait (a write stall).
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
    uint64_t number = 0;
    std::shared_ptr<LogWriter> new_log;
    if (s.ok()) s = NewLogFile(&number, &new_log);
    l.lock();
    if (!s.ok()) {
      bg_error_ = s;
      return Status::IOError(s.message());
    }
    imm_ = std::move(mem_);
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
  if (s.ok() && options.sync) s = log_->Sync();
  if (!s.ok()) {
    // We no longer know what the WAL contains, so stop accepting writes until reopen.
    std::lock_guard<std::mutex> l(mutex_);
    bg_error_ = s;
    return Status::IOError(s.message());
  }
  s = InsertInto(*batch, mem_.get());
  if (!s.ok()) return s;
  stat_user_bytes_ += batch->ApproximateSize();
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

// ---------------------------------------------------------------- read path

namespace {

bool InRange(std::string_view user_key, const FileMeta& f) {
  return user_key >= ExtractUserKey(f.smallest) && user_key <= ExtractUserKey(f.largest);
}

}  // namespace

Status Engine::Get(std::string_view key, std::string* value) {
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<const Version> version;
  SequenceNumber snapshot;
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (closed_) return Status::InvalidArgument("database is closed");
    mem = mem_;
    imm = imm_;
    version = versions_->current();
    snapshot = last_visible_sequence_.load(std::memory_order_acquire);
  }
  // Newest data first: memtable, immutable memtable, L0 newest to oldest, then L1.
  for (const MemTable* m : {mem.get(), imm.get()}) {
    if (m == nullptr) continue;
    switch (m->Get(key, snapshot, value)) {
      case MemTable::LookupResult::kFound:
        return Status::OK();
      case MemTable::LookupResult::kDeleted:
        return Status::NotFound();
      case MemTable::LookupResult::kNotPresent:
        break;
    }
  }
  const std::string lookup = MakeLookupKey(key, snapshot);
  auto search = [&](const FileMeta& f, bool* done) -> Status {
    *done = false;
    const TableReader& table = f.table->reader();
    if (table.has_filter()) {
      stat_bloom_checks_.fetch_add(1, std::memory_order_relaxed);
      if (!table.KeyMayMatch(key)) {
        stat_bloom_useful_.fetch_add(1, std::memory_order_relaxed);
        return Status::OK();
      }
    }
    TableReader::GetResult result;
    Status s = table.Get(lookup, &result, value);
    if (!s.ok()) return s;
    *done = result != TableReader::GetResult::kNotPresent;
    return result == TableReader::GetResult::kDeleted ? Status::NotFound() : Status::OK();
  };
  for (const auto& f : version->files[0]) {
    if (!InRange(key, *f)) continue;
    bool done;
    Status s = search(*f, &done);
    if (!s.ok() || done) return s;
  }
  // L1 files are disjoint and sorted: at most one can hold the key.
  const auto& l1 = version->files[1];
  auto it = std::lower_bound(l1.begin(), l1.end(), key, [](const auto& f, std::string_view k) {
    return ExtractUserKey(f->largest) < k;
  });
  if (it != l1.end() && InRange(key, **it)) {
    bool done;
    Status s = search(**it, &done);
    if (!s.ok() || done) return s;
  }
  return Status::NotFound();
}

// ---------------------------------------------------------------- background work

void Engine::BackgroundLoop() {
  std::unique_lock<std::mutex> l(mutex_);
  while (true) {
    bg_work_cv_.wait(l, [&] { return shutting_down_ || (bg_error_.ok() && imm_ != nullptr); });
    if (shutting_down_) break;  // an unflushed imm_ is still in its WAL and is replayed on open
    l.unlock();
    Status s = FlushImmutable();
    l.lock();
    if (!s.ok() && bg_error_.ok()) {
      Log(LogLevel::kError, "background flush failed: " + s.ToString());
      bg_error_ = s;
    }
    bg_done_cv_.notify_all();
  }
}

Status Engine::FlushImmutable() {
  Hook("flush:begin");
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
  }
  stat_flushes_++;
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
  j["flushes"] = stat_flushes_.load();
  j["flush_bytes_written"] = stat_flush_bytes_.load();
  j["user_bytes_written"] = stat_user_bytes_.load();
  j["bloom_checks"] = stat_bloom_checks_.load();
  j["bloom_useful"] = stat_bloom_useful_.load();
  j["stall_micros"] = stat_stall_micros_.load();
  j["block_cache_hits"] = cache_ ? cache_->hits() : 0;
  j["block_cache_misses"] = cache_ ? cache_->misses() : 0;
  return j;
}

}  // namespace docstore::kv
