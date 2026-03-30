#include "kv/table_reader.h"

#include <algorithm>

#include "kv/block_cache.h"
#include "kv/bloom.h"
#include "kv/table_format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace docstore::kv {
namespace {

// Reads [offset, offset + size) plus its CRC and verifies it.
Status ReadCheckedBlock(const RandomAccessFile& file, uint64_t offset, uint64_t size, bool verify,
                        std::string* out) {
  Status s = file.Read(offset, size + kBlockTrailerSize, out);
  if (!s.ok()) return s;
  if (out->size() != size + kBlockTrailerSize) return Status::Corruption("truncated block read");
  if (verify) {
    uint32_t stored = DecodeFixed32(out->data() + size);
    if (crc32c::Value(out->data(), size) != stored) {
      return Status::Corruption("block checksum mismatch at offset " + std::to_string(offset));
    }
  }
  out->resize(size);
  return Status::OK();
}

}  // namespace

Status TableReader::Open(const Options& options, std::unique_ptr<RandomAccessFile> file,
                         uint64_t file_size, uint64_t file_number, BlockCache* cache,
                         std::unique_ptr<TableReader>* out) {
  if (file_size < kFooterSize) return Status::Corruption("file too short to be an sstable");
  std::string footer;
  Status s = file->Read(file_size - kFooterSize, kFooterSize, &footer);
  if (!s.ok()) return s;
  if (footer.size() != kFooterSize) return Status::Corruption("truncated sstable footer");
  if (DecodeFixed64(footer.data() + 40) != kTableMagic) return Status::Corruption("bad magic");
  if (crc32c::Value(footer.data(), 36) != DecodeFixed32(footer.data() + 36)) {
    return Status::Corruption("footer checksum mismatch");
  }
  if (DecodeFixed32(footer.data() + 32) != kTableFormatVersion) {
    return Status::Corruption("unsupported sstable format version");
  }
  const uint64_t index_offset = DecodeFixed64(footer.data());
  const uint64_t index_size = DecodeFixed64(footer.data() + 8);
  const uint64_t filter_offset = DecodeFixed64(footer.data() + 16);
  const uint64_t filter_size = DecodeFixed64(footer.data() + 24);
  const uint64_t body_end = file_size - kFooterSize;
  // Layout is filter then index then footer; check without overflow.
  if (filter_offset > body_end || filter_size + kBlockTrailerSize > body_end - filter_offset ||
      index_offset != filter_offset + filter_size + kBlockTrailerSize ||
      index_size + kBlockTrailerSize != body_end - index_offset) {
    return Status::Corruption("bad block handles in sstable footer");
  }

  std::unique_ptr<TableReader> table(new TableReader());
  table->paranoid_checks_ = options.paranoid_checks;
  table->file_number_ = file_number;
  table->cache_ = cache;

  // Index and filter are always verified: a bad index would send reads to the wrong block.
  std::string index;
  s = ReadCheckedBlock(*file, index_offset, index_size, /*verify=*/true, &index);
  if (s.ok()) s = ReadCheckedBlock(*file, filter_offset, filter_size, true, &table->filter_);
  if (!s.ok()) return s;

  std::string_view in = index;
  uint64_t expected_offset = 0;
  while (!in.empty()) {
    IndexEntry e;
    std::string_view key;
    if (!GetLengthPrefixed(&in, &key) || key.size() < kInternalKeyTrailer ||
        !GetFixed64(&in, &e.offset) || !GetFixed32(&in, &e.size) || e.offset != expected_offset) {
      return Status::Corruption("bad sstable index entry");
    }
    e.last_key.assign(key);
    expected_offset = e.offset + e.size + kBlockTrailerSize;
    table->index_.push_back(std::move(e));
  }
  if (expected_offset != filter_offset) return Status::Corruption("index does not cover data");
  table->file_ = std::move(file);
  *out = std::move(table);
  return Status::OK();
}

bool TableReader::KeyMayMatch(std::string_view user_key) const {
  return !has_filter() || BloomKeyMayMatch(filter_, user_key);
}

size_t TableReader::FindBlock(std::string_view ikey) const {
  auto it = std::lower_bound(index_.begin(), index_.end(), ikey,
                             [](const IndexEntry& e, std::string_view k) {
                               return CompareInternalKeys(e.last_key, k) < 0;
                             });
  return static_cast<size_t>(it - index_.begin());
}

Status TableReader::ReadBlock(size_t i, std::shared_ptr<const Block>* out) const {
  const IndexEntry& e = index_[i];
  if (cache_ != nullptr) {
    *out = cache_->Lookup(file_number_, e.offset);
    if (*out) return Status::OK();
  }
  std::string contents;
  Status s = ReadCheckedBlock(*file_, e.offset, e.size, paranoid_checks_, &contents);
  if (!s.ok()) return s;
  auto block = std::make_shared<const Block>(std::move(contents));
  if (cache_ != nullptr) cache_->Insert(file_number_, e.offset, block);
  *out = std::move(block);
  return Status::OK();
}

Status TableReader::Get(std::string_view lookup_key, GetResult* result, std::string* value) const {
  *result = GetResult::kNotPresent;
  size_t i = FindBlock(lookup_key);
  if (i == index_.size()) return Status::OK();
  std::shared_ptr<const Block> block;
  Status s = ReadBlock(i, &block);
  if (!s.ok()) return s;
  auto it = Block::NewIterator(block);
  it->Seek(lookup_key);
  if (!it->Valid()) return it->status();
  ParsedInternalKey parsed;
  if (!ParseInternalKey(it->key(), &parsed)) return Status::Corruption("bad internal key");
  if (parsed.user_key != ExtractUserKey(lookup_key)) return Status::OK();
  if (parsed.type == ValueType::kDeletion) {
    *result = GetResult::kDeleted;
  } else {
    *result = GetResult::kFound;
    value->assign(it->value());
  }
  return Status::OK();
}

// Two-level iterator: walks the in-memory index and opens one data block at a time.
class TableIterator final : public Iterator {
 public:
  explicit TableIterator(const TableReader* table) : table_(table) {}

  bool Valid() const override { return block_iter_ && block_iter_->Valid(); }
  void SeekToFirst() override {
    OpenBlock(0);
    if (block_iter_) block_iter_->SeekToFirst();
    SkipEmptyBlocks();
  }
  void Seek(std::string_view target) override {
    OpenBlock(table_->FindBlock(target));
    if (block_iter_) block_iter_->Seek(target);
    SkipEmptyBlocks();
  }
  void Next() override {
    block_iter_->Next();
    SkipEmptyBlocks();
  }
  std::string_view key() const override { return block_iter_->key(); }
  std::string_view value() const override { return block_iter_->value(); }
  Status status() const override { return status_; }

 private:
  void OpenBlock(size_t i) {
    block_iter_.reset();
    block_index_ = i;
    if (!status_.ok() || i >= table_->num_blocks()) return;
    std::shared_ptr<const Block> block;
    status_ = table_->ReadBlock(i, &block);
    if (status_.ok()) block_iter_ = Block::NewIterator(std::move(block));
  }

  void SkipEmptyBlocks() {
    while (block_iter_ && !block_iter_->Valid()) {
      if (!block_iter_->status().ok()) {
        status_ = block_iter_->status();
        block_iter_.reset();
        return;
      }
      OpenBlock(block_index_ + 1);
      if (block_iter_) block_iter_->SeekToFirst();
    }
  }

  const TableReader* table_;
  size_t block_index_ = 0;
  std::unique_ptr<Iterator> block_iter_;
  Status status_;
};

std::unique_ptr<Iterator> TableReader::NewIterator() const {
  return std::make_unique<TableIterator>(this);
}

}  // namespace docstore::kv
