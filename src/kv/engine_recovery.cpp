// Engine::Open's recovery sequence and obsolete-file cleanup. Split from
// engine.cpp to keep each file small.

#include <algorithm>
#include <set>
#include <vector>

#include "env/env.h"
#include "kv/engine.h"
#include "kv/filename.h"
#include "kv/log_reader.h"
#include "kv/log_writer.h"
#include "kv/memtable.h"
#include "kv/version_set.h"
#include "util/logger.h"

namespace docstore::kv {

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

}  // namespace docstore::kv
