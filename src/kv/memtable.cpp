#include "kv/memtable.h"

#include "util/coding.h"

namespace docstore::kv {

std::string_view GetLengthPrefixedKey(const char* entry) {
  // Entries were written by Add(), so the varint is well formed; 5 bytes bound a varint32.
  std::string_view in(entry, kMaxVarint32Bytes);
  uint32_t len = 0;
  (void)GetVarint32(&in, &len);
  return std::string_view(in.data(), len);
}

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  return CompareInternalKeys(GetLengthPrefixedKey(a), GetLengthPrefixedKey(b));
}

MemTable::MemTable() : table_(KeyComparator(), &arena_) {}

void MemTable::Add(SequenceNumber seq, ValueType type, std::string_view user_key,
                   std::string_view value) {
  size_t ikey_len = user_key.size() + kInternalKeyTrailer;
  size_t encoded_len =
      VarintLength(ikey_len) + ikey_len + VarintLength(value.size()) + value.size();
  char* buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint64(buf, ikey_len);
  // string_view::copy, unlike memcpy, is defined for an empty view with a null data().
  p += user_key.copy(p, user_key.size());
  EncodeFixed64(p, PackSequenceAndType(seq, type));
  p += kInternalKeyTrailer;
  p = EncodeVarint64(p, value.size());
  value.copy(p, value.size());
  table_.Insert(buf);
  num_entries_.fetch_add(1, std::memory_order_relaxed);
}

namespace {

// Encodes an internal key in memtable entry form so it can be used as a skiplist seek target.
std::string EncodeSeekTarget(std::string_view ikey) {
  std::string buf;
  PutVarint32(&buf, static_cast<uint32_t>(ikey.size()));
  buf.append(ikey);
  return buf;
}

std::string_view EntryValue(const char* entry) {
  std::string_view key = GetLengthPrefixedKey(entry);
  std::string_view in(key.data() + key.size(), kMaxVarint32Bytes);
  uint32_t len = 0;
  (void)GetVarint32(&in, &len);
  return std::string_view(in.data(), len);
}

}  // namespace

MemTable::LookupResult MemTable::Get(std::string_view user_key, SequenceNumber snapshot,
                                     std::string* value) const {
  std::string target = EncodeSeekTarget(MakeLookupKey(user_key, snapshot));
  Table::Iterator it(&table_);
  it.Seek(target.data());
  if (!it.Valid()) return LookupResult::kNotPresent;
  ParsedInternalKey parsed;
  if (!ParseInternalKey(GetLengthPrefixedKey(it.key()), &parsed) || parsed.user_key != user_key) {
    return LookupResult::kNotPresent;
  }
  if (parsed.type == ValueType::kDeletion) return LookupResult::kDeleted;
  std::string_view v = EntryValue(it.key());
  value->assign(v.data(), v.size());
  return LookupResult::kFound;
}

class MemTableIterator final : public Iterator {
 public:
  explicit MemTableIterator(const MemTable::Table* table) : it_(table) {}

  bool Valid() const override { return it_.Valid(); }
  void SeekToFirst() override { it_.SeekToFirst(); }
  void Seek(std::string_view target) override {
    scratch_ = EncodeSeekTarget(target);
    it_.Seek(scratch_.data());
  }
  void Next() override { it_.Next(); }
  std::string_view key() const override { return GetLengthPrefixedKey(it_.key()); }
  std::string_view value() const override { return EntryValue(it_.key()); }
  Status status() const override { return Status::OK(); }

 private:
  MemTable::Table::Iterator it_;
  std::string scratch_;
};

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  return std::make_unique<MemTableIterator>(&table_);
}

}  // namespace docstore::kv
