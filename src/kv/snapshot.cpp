#include "kv/snapshot.h"

#include <algorithm>
#include <vector>

#include "kv/iterators.h"
#include "kv/memtable.h"
#include "kv/table_reader.h"
#include "kv/version_set.h"

namespace docstore::kv {
namespace {

bool InRange(std::string_view user_key, const FileMeta& f) {
  return user_key >= ExtractUserKey(f.smallest) && user_key <= ExtractUserKey(f.largest);
}

}  // namespace

Status Snapshot::Get(std::string_view key, std::string* value) const {
  // Newest data first: memtable, immutable memtable, L0 newest to oldest, then L1.
  for (const MemTable* m : {mem_.get(), imm_.get()}) {
    if (m == nullptr) continue;
    switch (m->Get(key, sequence_, value)) {
      case MemTable::LookupResult::kFound:
        return Status::OK();
      case MemTable::LookupResult::kDeleted:
        return Status::NotFound();
      case MemTable::LookupResult::kNotPresent:
        break;
    }
  }
  const std::string lookup = MakeLookupKey(key, sequence_);
  // Returns true when the table decided the answer (found or deleted).
  auto search = [&](const FileMeta& f, Status* s) -> bool {
    const TableReader& table = f.table->reader();
    if (table.has_filter()) {
      stats_->bloom_checks.fetch_add(1, std::memory_order_relaxed);
      if (!table.KeyMayMatch(key)) {
        stats_->bloom_useful.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
    }
    TableReader::GetResult result;
    *s = table.Get(lookup, &result, value);
    if (!s->ok()) return true;
    if (result == TableReader::GetResult::kDeleted) *s = Status::NotFound();
    return result != TableReader::GetResult::kNotPresent;
  };
  Status s;
  for (const auto& f : version_->files[0]) {
    if (InRange(key, *f) && search(*f, &s)) return s;
  }
  // L1 files are disjoint and sorted: at most one can hold the key.
  const auto& l1 = version_->files[1];
  auto it = std::lower_bound(l1.begin(), l1.end(), key, [](const auto& f, std::string_view k) {
    return ExtractUserKey(f->largest) < k;
  });
  if (it != l1.end() && InRange(key, **it) && search(**it, &s)) return s;
  return Status::NotFound();
}

std::unique_ptr<Iterator> Snapshot::NewIterator(const ReadOptions& options) const {
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(mem_->NewIterator());
  if (imm_) children.push_back(imm_->NewIterator());
  for (const auto& f : version_->files[0]) {
    // Skip L0 tables that cannot overlap the requested range.
    if (!options.upper.empty() && ExtractUserKey(f->smallest) >= std::string_view(options.upper)) {
      continue;
    }
    if (ExtractUserKey(f->largest) < std::string_view(options.lower)) continue;
    children.push_back(f->table->reader().NewIterator());
  }
  children.push_back(NewLevelIterator(&version_->files[1]));
  auto it = NewDBIterator(NewMergingIterator(std::move(children)), sequence_, options.lower,
                          options.upper, shared_from_this());
  it->SeekToFirst();
  return it;
}

}  // namespace docstore::kv
