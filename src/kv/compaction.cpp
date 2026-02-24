#include "kv/compaction.h"

#include "env/env.h"
#include "kv/filename.h"
#include "kv/iterators.h"
#include "kv/table_builder.h"
#include "kv/table_reader.h"
#include "kv/version_set.h"

namespace docstore::kv {

uint64_t Compaction::InputBytes() const {
  uint64_t total = 0;
  for (const auto& level : inputs) {
    for (const auto& f : level) total += f->file_size;
  }
  return total;
}

Compaction PickCompaction(const Version& version, bool full) {
  Compaction c;
  c.inputs[0] = version.files[0];
  if (full) {
    c.inputs[1] = version.files[1];
    return c;
  }
  if (c.inputs[0].empty()) return c;
  std::string_view smallest = ExtractUserKey(c.inputs[0][0]->smallest);
  std::string_view largest = ExtractUserKey(c.inputs[0][0]->largest);
  for (const auto& f : c.inputs[0]) {
    smallest = std::min(smallest, ExtractUserKey(f->smallest));
    largest = std::max(largest, ExtractUserKey(f->largest));
  }
  // Every L1 file that could hold an older version of a key in L0 must be an input, or
  // dropping a tombstone could resurrect that older version.
  for (const auto& f : version.files[1]) {
    if (ExtractUserKey(f->largest) >= smallest && ExtractUserKey(f->smallest) <= largest) {
      c.inputs[1].push_back(f);
    }
  }
  return c;
}

namespace {

class OutputWriter {
 public:
  OutputWriter(const Options& options, Env* env, const std::string& dbname, VersionSet* versions,
               CompactionResult* result)
      : options_(options), env_(env), dbname_(dbname), versions_(versions), result_(result) {}

  Status Add(std::string_view key, std::string_view value) {
    if (!builder_) {
      FileMeta meta;
      meta.number = versions_->NewFileNumber();
      std::unique_ptr<WritableFile> file;
      Status s = env_->NewWritableFile(TableFileName(dbname_, meta.number), &file);
      if (!s.ok()) return s;
      written_.push_back(meta.number);
      builder_ = std::make_unique<TableBuilder>(options_, std::move(file));
      current_ = std::move(meta);
    }
    builder_->Add(key, value);
    // A user key never spans two outputs: only one entry per user key survives the merge.
    if (builder_->FileSize() >= options_.target_file_size) return FinishOutput();
    return Status::OK();
  }

  Status FinishOutput() {
    if (!builder_) return Status::OK();
    Status s = builder_->Finish();
    current_.file_size = builder_->FileSize();
    current_.smallest = builder_->smallest_key();
    current_.largest = builder_->largest_key();
    builder_.reset();
    if (!s.ok()) return s;
    result_->bytes_written += current_.file_size;
    result_->outputs.push_back(std::move(current_));
    return Status::OK();
  }

  // Makes the outputs' names durable and opens them for reading.
  Status Install() {
    Status s = env_->SyncDir(dbname_);
    for (auto& meta : result_->outputs) {
      if (s.ok()) s = versions_->OpenTable(&meta);
    }
    return s;
  }

  void DeleteAll() {
    builder_.reset();
    result_->outputs.clear();
    for (uint64_t n : written_) (void)env_->DeleteFile(TableFileName(dbname_, n));
  }

 private:
  const Options& options_;
  Env* env_;
  const std::string& dbname_;
  VersionSet* versions_;
  CompactionResult* result_;
  std::unique_ptr<TableBuilder> builder_;
  FileMeta current_;
  std::vector<uint64_t> written_;
};

}  // namespace

Status RunCompaction(const Options& options, Env* env, const std::string& dbname,
                     VersionSet* versions, const Compaction& c,
                     const std::function<bool()>& between_entries, CompactionResult* result,
                     bool* aborted) {
  *aborted = false;
  std::vector<std::unique_ptr<Iterator>> children;
  for (const auto& level : c.inputs) {
    for (const auto& f : level) children.push_back(f->table->reader().NewIterator());
  }
  auto merged = NewMergingIterator(std::move(children));
  OutputWriter out(options, env, dbname, versions, result);
  const CompactionFilter* filter = options.compaction_filter.get();

  Status s;
  std::string current_user_key;
  bool has_current = false;
  uint64_t n = 0;
  for (merged->SeekToFirst(); merged->Valid() && s.ok(); merged->Next()) {
    if (++n % 256 == 0 && !between_entries()) {
      *aborted = true;
      break;
    }
    ParsedInternalKey p;
    if (!ParseInternalKey(merged->key(), &p)) {
      s = Status::Corruption("bad internal key during compaction");
      break;
    }
    // Entries arrive newest first within a user key; only the first one can survive.
    if (has_current && p.user_key == current_user_key) {
      result->entries_dropped++;
      continue;
    }
    current_user_key.assign(p.user_key);
    has_current = true;
    if (p.type == ValueType::kDeletion || (filter != nullptr && filter->ShouldDrop(p.user_key))) {
      result->entries_dropped++;
      continue;
    }
    s = out.Add(merged->key(), merged->value());
  }
  if (s.ok() && !*aborted) s = merged->status();
  if (s.ok() && !*aborted) s = out.FinishOutput();
  if (s.ok() && !*aborted) s = out.Install();
  if (!s.ok() || *aborted) out.DeleteAll();
  return s;
}

}  // namespace docstore::kv
