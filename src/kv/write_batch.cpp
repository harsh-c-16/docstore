#include "kv/write_batch.h"

#include "kv/memtable.h"
#include "util/coding.h"

namespace docstore::kv {

void WriteBatch::Clear() { rep_.assign(kHeaderSize, '\0'); }

uint32_t WriteBatch::Count() const { return DecodeFixed32(rep_.data() + 8); }

SequenceNumber WriteBatch::Sequence() const { return DecodeFixed64(rep_.data()); }

void WriteBatch::SetSequence(SequenceNumber seq) { EncodeFixed64(rep_.data(), seq); }

void WriteBatch::Put(std::string_view key, std::string_view value) {
  EncodeFixed32(rep_.data() + 8, Count() + 1);
  rep_.push_back(static_cast<char>(ValueType::kValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  EncodeFixed32(rep_.data() + 8, Count() + 1);
  rep_.push_back(static_cast<char>(ValueType::kDeletion));
  PutLengthPrefixed(&rep_, key);
}

Status WriteBatch::SetContents(std::string_view contents) {
  if (contents.size() < kHeaderSize) return Status::Corruption("write batch too small");
  rep_.assign(contents);
  return Status::OK();
}

Status WriteBatch::Iterate(Handler* handler) const {
  std::string_view in(rep_);
  in.remove_prefix(kHeaderSize);
  uint32_t found = 0;
  while (!in.empty()) {
    auto type = static_cast<ValueType>(in[0]);
    in.remove_prefix(1);
    std::string_view key, value;
    if (!GetLengthPrefixed(&in, &key)) return Status::Corruption("bad write batch key");
    if (type == ValueType::kValue) {
      if (!GetLengthPrefixed(&in, &value)) return Status::Corruption("bad write batch value");
      handler->Put(key, value);
    } else if (type == ValueType::kDeletion) {
      handler->Delete(key);
    } else {
      return Status::Corruption("unknown write batch record type");
    }
    found++;
  }
  if (found != Count()) return Status::Corruption("write batch count mismatch");
  return Status::OK();
}

namespace {

class MemTableInserter final : public WriteBatch::Handler {
 public:
  MemTableInserter(SequenceNumber seq, MemTable* mem) : seq_(seq), mem_(mem) {}
  void Put(std::string_view key, std::string_view value) override {
    mem_->Add(seq_++, ValueType::kValue, key, value);
  }
  void Delete(std::string_view key) override { mem_->Add(seq_++, ValueType::kDeletion, key, {}); }

 private:
  SequenceNumber seq_;
  MemTable* mem_;
};

}  // namespace

Status InsertInto(const WriteBatch& batch, MemTable* mem) {
  MemTableInserter inserter(batch.Sequence(), mem);
  return batch.Iterate(&inserter);
}

}  // namespace docstore::kv
