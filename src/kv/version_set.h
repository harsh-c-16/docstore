#pragma once

// Versions: immutable snapshots of the live file set, plus the MANIFEST that persists them.
//
// File lifecycle is RAII (DESIGN.md D12): a Version holds shared_ptr<FileMeta>, which holds the
// open TableFile. When an edit removes a file, its TableFile is marked obsolete, and the file is
// deleted by ~TableFile once the last Version that lists it is released. A scan that holds an
// old Version therefore never has its files deleted underneath it.

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "docstore/options.h"
#include "env/env.h"
#include "kv/log_writer.h"
#include "kv/table_reader.h"
#include "kv/version_edit.h"

namespace docstore::kv {

class BlockCache;

class TableFile {
 public:
  TableFile(Env* env, std::string path, std::unique_ptr<TableReader> reader)
      : env_(env), path_(std::move(path)), reader_(std::move(reader)) {}
  ~TableFile();
  TableFile(const TableFile&) = delete;
  TableFile& operator=(const TableFile&) = delete;

  const TableReader& reader() const { return *reader_; }
  void MarkObsolete() { obsolete_.store(true, std::memory_order_relaxed); }

 private:
  Env* env_;
  std::string path_;
  std::unique_ptr<TableReader> reader_;
  std::atomic<bool> obsolete_{false};
};

struct Version {
  // L0: newest first (ranges may overlap). L1: sorted by smallest key, non-overlapping.
  std::vector<std::shared_ptr<const FileMeta>> files[kNumLevels];

  int NumFiles(int level) const { return static_cast<int>(files[level].size()); }
  uint64_t LevelBytes(int level) const;
};

class VersionSet {
 public:
  VersionSet(std::string dbname, const Options& options, Env* env, BlockCache* cache);

  // Loads the state named by CURRENT and opens every table (validating footers and sizes).
  Status Recover();
  // Adds files to the current version without writing the MANIFEST (recovery-time flushes; the
  // snapshot written next by WriteSnapshot records them).
  void ApplyWithoutLogging(const VersionEdit& edit);
  // Writes a fresh MANIFEST holding the whole current state, then atomically points CURRENT at
  // it (CURRENT.tmp, sync, rename, sync dir). Later LogAndApply calls append to this MANIFEST.
  Status WriteSnapshot(uint64_t log_number, SequenceNumber last_sequence);
  // Appends the edit to the MANIFEST and syncs it, then installs the new Version under *mu.
  // Only the background thread calls this once the database is open, so the MANIFEST write
  // itself needs no lock.
  Status LogAndApply(VersionEdit* edit, SequenceNumber last_sequence, std::mutex* mu);

  // Opens meta's table file and sets meta->table.
  Status OpenTable(FileMeta* meta);

  // Caller holds the engine mutex, or is the background thread (the only writer of current_).
  std::shared_ptr<const Version> current() const { return current_; }

  uint64_t NewFileNumber() { return next_file_number_.fetch_add(1); }
  void MarkFileNumberUsed(uint64_t number);
  uint64_t log_number() const { return log_number_; }
  SequenceNumber last_sequence() const { return last_sequence_; }
  uint64_t manifest_number() const { return manifest_number_; }

 private:
  std::shared_ptr<const Version> Apply(const Version& base, const VersionEdit& edit) const;

  const std::string dbname_;
  const Options& options_;
  Env* const env_;
  BlockCache* const cache_;

  std::shared_ptr<const Version> current_;
  std::atomic<uint64_t> next_file_number_{2};
  uint64_t log_number_ = 0;
  SequenceNumber last_sequence_ = 0;
  uint64_t manifest_number_ = 0;
  std::unique_ptr<LogWriter> manifest_;
};

}  // namespace docstore::kv
