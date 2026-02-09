#include "kv/engine.h"

#include <algorithm>
#include <vector>

#include "env/env.h"
#include "kv/filename.h"
#include "kv/log_reader.h"
#include "kv/log_writer.h"
#include "kv/memtable.h"
#include "util/logger.h"

namespace docstore::kv {

Engine::Engine(const Options& options, std::string dbname)
    : options_(options),
      env_(options.env != nullptr ? options.env : Env::Default()),
      dbname_(std::move(dbname)),
      mem_(std::make_shared<MemTable>()) {}

Engine::~Engine() { (void)Close(); }

Status Engine::Open(const Options& options, const std::string& dbname,
                    std::unique_ptr<Engine>* out) {
  std::unique_ptr<Engine> engine(new Engine(options, dbname));
  Status s = engine->Recover();
  if (!s.ok()) return s;
  *out = std::move(engine);
  return Status::OK();
}

Status Engine::Recover() {
  Status s = env_->CreateDir(dbname_);
  if (s.ok()) s = env_->LockFile(LockFileName(dbname_), &lock_);
  if (!s.ok()) return s;

  std::vector<std::string> children;
  s = env_->GetChildren(dbname_, &children);
  if (!s.ok()) return s;
  std::vector<uint64_t> logs;
  for (const auto& name : children) {
    uint64_t number = 0;
    FileType type = ParseFileName(name, &number);
    if (type == FileType::kUnknown || type == FileType::kCurrent || type == FileType::kLock) {
      continue;
    }
    // Never reuse a number that is already on disk.
    next_file_number_ = std::max(next_file_number_, number + 1);
    if (type == FileType::kLog) logs.push_back(number);
  }
  std::sort(logs.begin(), logs.end());
  for (size_t i = 0; i < logs.size(); i++) {
    s = ReplayLog(logs[i], i + 1 == logs.size());
    if (!s.ok()) return s;
  }
  last_visible_sequence_.store(last_sequence_, std::memory_order_release);
  return CreateNewLog();
}

Status Engine::ReplayLog(uint64_t number, bool newest) {
  std::string fname = LogFileName(dbname_, number);
  std::string contents;
  Status s = ReadFileToString(env_, fname, &contents);
  if (!s.ok()) return s;
  LogReader reader(contents);
  std::string_view record;
  WriteBatch batch;
  while (reader.ReadRecord(&record)) {
    s = batch.SetContents(record);
    if (s.ok()) s = InsertInto(batch, mem_.get());
    if (!s.ok()) return Status::Corruption(fname + ": " + s.message());
    last_sequence_ = std::max(last_sequence_, batch.Sequence() + batch.Count() - 1);
  }
  if (reader.problem() == LogReader::Problem::kNone) return Status::OK();
  if (newest) {
    // A crash in the middle of an append leaves a torn record at the end of the newest WAL.
    // Its write was never acknowledged, so recovery stops at the last good record.
    Log(LogLevel::kWarn, fname + ": ignoring torn tail after byte " +
                             std::to_string(reader.valid_prefix_length()) + " (" +
                             reader.status().message() + ")");
    return Status::OK();
  }
  if (options_.paranoid_checks) return Status::Corruption(fname + ": " + reader.status().message());
  Log(LogLevel::kError, fname + ": skipping rest of file: " + reader.status().message());
  return Status::OK();
}

Status Engine::CreateNewLog() {
  uint64_t number = next_file_number_++;
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(LogFileName(dbname_, number), &file);
  // The new file's directory entry must be durable before any sync=true write in it is
  // acknowledged, otherwise power loss could take the whole file with it.
  if (s.ok()) s = env_->SyncDir(dbname_);
  if (!s.ok()) return s;
  log_ = std::make_shared<LogWriter>(std::move(file));
  log_number_ = number;
  return Status::OK();
}

Status Engine::Close() {
  std::lock_guard<std::mutex> w(write_mutex_);
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (closed_) return Status::OK();
    closed_ = true;
  }
  Status s;
  if (log_) s = log_->Sync();  // make sync=false writes durable on a clean shutdown
  log_.reset();
  lock_.reset();
  return s;
}

Status Engine::Write(const WriteOptions& options, WriteBatch* batch) {
  std::lock_guard<std::mutex> w(write_mutex_);
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (closed_) return Status::InvalidArgument("database is closed");
    if (!bg_error_.ok()) return Status::ReadOnly(bg_error_.ToString());
  }
  if (batch->Count() == 0) return Status::OK();

  batch->SetSequence(last_sequence_ + 1);
  last_sequence_ += batch->Count();
  Status s = log_->AddRecord(batch->Contents());
  if (s.ok() && options.sync) s = log_->Sync();
  if (!s.ok()) {
    // We no longer know what the WAL contains, so stop accepting writes until reopen.
    std::lock_guard<std::mutex> l(mutex_);
    bg_error_ = s;
    return Status::IOError(s.message());
  }
  s = InsertInto(*batch, mem_.get());
  if (!s.ok()) return s;
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

Status Engine::Get(std::string_view key, std::string* value) {
  std::shared_ptr<MemTable> mem;
  SequenceNumber snapshot;
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (closed_) return Status::InvalidArgument("database is closed");
    mem = mem_;
    snapshot = last_visible_sequence_.load(std::memory_order_acquire);
  }
  switch (mem->Get(key, snapshot, value)) {
    case MemTable::LookupResult::kFound:
      return Status::OK();
    case MemTable::LookupResult::kDeleted:
    case MemTable::LookupResult::kNotPresent:
      break;
  }
  return Status::NotFound();
}

}  // namespace docstore::kv
