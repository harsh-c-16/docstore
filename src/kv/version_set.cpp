#include "kv/version_set.h"

#include <algorithm>
#include <map>

#include "kv/filename.h"
#include "kv/log_reader.h"
#include "util/logger.h"

namespace docstore::kv {

TableFile::~TableFile() {
  if (!obsolete_.load(std::memory_order_relaxed)) return;
  reader_.reset();  // close the file before unlinking it
  Status s = env_->DeleteFile(path_);
  // A failed delete only leaks disk space: the next Open removes unreferenced tables.
  if (!s.ok()) Log(LogLevel::kWarn, "could not delete obsolete table: " + s.ToString());
}

uint64_t Version::LevelBytes(int level) const {
  uint64_t total = 0;
  for (const auto& f : files[level]) total += f->file_size;
  return total;
}

VersionSet::VersionSet(std::string dbname, const Options& options, Env* env, BlockCache* cache)
    : dbname_(std::move(dbname)),
      options_(options),
      env_(env),
      cache_(cache),
      current_(std::make_shared<const Version>()) {}

void VersionSet::MarkFileNumberUsed(uint64_t number) {
  uint64_t next = next_file_number_.load();
  while (next <= number && !next_file_number_.compare_exchange_weak(next, number + 1)) {
  }
}

Status VersionSet::OpenTable(FileMeta* meta) {
  std::string path = TableFileName(dbname_, meta->number);
  uint64_t size = 0;
  Status s = env_->GetFileSize(path, &size);
  if (!s.ok()) return Status::Corruption("missing table file " + path + ": " + s.ToString());
  if (size != meta->file_size) return Status::Corruption("table file has wrong size: " + path);
  std::unique_ptr<RandomAccessFile> file;
  s = env_->NewRandomAccessFile(path, &file);
  std::unique_ptr<TableReader> reader;
  if (s.ok()) s = TableReader::Open(options_, std::move(file), size, meta->number, cache_, &reader);
  if (!s.ok()) return Status::Corruption(path + ": " + s.message());
  meta->table = std::make_shared<TableFile>(env_, path, std::move(reader));
  return Status::OK();
}

std::shared_ptr<const Version> VersionSet::Apply(const Version& base,
                                                 const VersionEdit& edit) const {
  auto v = std::make_shared<Version>();
  for (int level = 0; level < kNumLevels; level++) {
    for (const auto& f : base.files[level]) {
      bool deleted = std::any_of(edit.deleted_files.begin(), edit.deleted_files.end(),
                                 [&](const auto& d) { return d.first == level && d.second == f->number; });
      if (!deleted) v->files[level].push_back(f);
    }
  }
  for (const auto& [level, meta] : edit.new_files) {
    v->files[level].push_back(std::make_shared<const FileMeta>(meta));
  }
  // L0 files only come from flushes, which run one at a time, so a higher number is newer.
  std::sort(v->files[0].begin(), v->files[0].end(),
            [](const auto& a, const auto& b) { return a->number > b->number; });
  std::sort(v->files[1].begin(), v->files[1].end(), [](const auto& a, const auto& b) {
    return CompareInternalKeys(a->smallest, b->smallest) < 0;
  });
  return v;
}

Status VersionSet::Recover() {
  std::string current;
  Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
  if (!s.ok()) return s;
  uint64_t number = 0;
  if (current.empty() || current.back() != '\n' ||
      ParseFileName(current.substr(0, current.size() - 1), &number) != FileType::kManifest) {
    return Status::Corruption("CURRENT does not name a manifest");
  }
  std::string manifest_path = dbname_ + "/" + current.substr(0, current.size() - 1);
  std::string contents;
  s = ReadFileToString(env_, manifest_path, &contents);
  if (!s.ok()) return Status::Corruption("cannot read manifest named by CURRENT: " + s.ToString());

  std::map<uint64_t, FileMeta> files[kNumLevels];
  std::optional<uint64_t> log_number, next_file;
  std::optional<SequenceNumber> last_seq;
  LogReader reader(contents);
  std::string_view record;
  while (reader.ReadRecord(&record)) {
    VersionEdit edit;
    s = edit.DecodeFrom(record);
    if (!s.ok()) return Status::Corruption(manifest_path + ": " + s.message());
    if (edit.log_number) log_number = edit.log_number;
    if (edit.next_file_number) next_file = edit.next_file_number;
    if (edit.last_sequence) last_seq = edit.last_sequence;
    for (const auto& [level, n] : edit.deleted_files) files[level].erase(n);
    for (const auto& [level, meta] : edit.new_files) files[level][meta.number] = meta;
  }
  if (reader.problem() != LogReader::Problem::kNone) {
    // See DESIGN.md D11: only an interrupted final append may be ignored.
    if (!reader.BadRecordIsTornTail()) {
      return Status::Corruption(manifest_path + ": " + reader.status().message());
    }
    Log(LogLevel::kWarn, manifest_path + ": ignoring torn final record");
  }
  if (!log_number || !next_file || !last_seq) {
    return Status::Corruption(manifest_path + ": missing required fields");
  }

  VersionEdit all;
  for (int level = 0; level < kNumLevels; level++) {
    for (auto& [n, meta] : files[level]) {
      s = OpenTable(&meta);
      if (!s.ok()) return s;
      all.AddFile(level, meta);
    }
  }
  current_ = Apply(Version(), all);
  log_number_ = *log_number;
  last_sequence_ = *last_seq;
  manifest_number_ = number;
  MarkFileNumberUsed(*next_file - 1);
  MarkFileNumberUsed(number);
  return Status::OK();
}

void VersionSet::ApplyWithoutLogging(const VersionEdit& edit) { current_ = Apply(*current_, edit); }

Status VersionSet::WriteSnapshot(uint64_t log_number, SequenceNumber last_sequence) {
  const uint64_t number = NewFileNumber();
  const std::string path = ManifestFileName(dbname_, number);
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(path, &file);
  if (!s.ok()) return s;
  auto writer = std::make_unique<LogWriter>(std::move(file));

  VersionEdit snapshot;
  snapshot.SetLogNumber(log_number);
  snapshot.SetNextFileNumber(next_file_number_.load());
  snapshot.SetLastSequence(last_sequence);
  for (int level = 0; level < kNumLevels; level++) {
    for (const auto& f : current_->files[level]) snapshot.AddFile(level, *f);
  }
  std::string record;
  snapshot.EncodeTo(&record);
  s = writer->AddRecord(record);
  if (s.ok()) s = writer->Sync();

  // Switch CURRENT atomically: a crash leaves either the old or the new name, never a mix.
  char name[32];
  std::snprintf(name, sizeof(name), "MANIFEST-%06llu\n", static_cast<unsigned long long>(number));
  const std::string tmp = CurrentFileName(dbname_) + ".tmp";
  if (s.ok()) s = WriteStringToFileSync(env_, name, tmp);
  if (s.ok()) s = env_->RenameFile(tmp, CurrentFileName(dbname_));
  // One directory sync makes both the new MANIFEST's name and the rename durable.
  if (s.ok()) s = env_->SyncDir(dbname_);
  if (!s.ok()) return s;

  manifest_ = std::move(writer);
  manifest_number_ = number;
  log_number_ = log_number;
  last_sequence_ = last_sequence;
  return Status::OK();
}

Status VersionSet::LogAndApply(VersionEdit* edit, SequenceNumber last_sequence, std::mutex* mu) {
  edit->SetNextFileNumber(next_file_number_.load());
  edit->SetLastSequence(last_sequence);
  std::shared_ptr<const Version> base = current_;  // keeps removed files alive until marked
  std::shared_ptr<const Version> next = Apply(*base, *edit);

  std::string record;
  edit->EncodeTo(&record);
  Status s = manifest_->AddRecord(record);
  if (s.ok()) s = manifest_->Sync();
  if (!s.ok()) return s;  // not installed; the caller turns this into a background error

  {
    std::lock_guard<std::mutex> l(*mu);
    current_ = next;
  }
  if (edit->log_number) log_number_ = *edit->log_number;
  last_sequence_ = last_sequence;
  // Only now that the edit is durable may the removed files go away.
  for (const auto& [level, number] : edit->deleted_files) {
    for (const auto& f : base->files[level]) {
      if (f->number == number) f->table->MarkObsolete();
    }
  }
  return Status::OK();
}

}  // namespace docstore::kv
