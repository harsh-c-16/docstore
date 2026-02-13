#include "kv/table_builder.h"

#include "kv/dbformat.h"
#include "kv/table_format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace docstore::kv {

TableBuilder::TableBuilder(const Options& options, std::unique_ptr<WritableFile> file)
    : block_size_(options.block_size),
      use_bloom_(options.bloom_bits_per_key > 0),
      file_(std::move(file)),
      bloom_(options.bloom_bits_per_key > 0 ? options.bloom_bits_per_key : 1) {}

void TableBuilder::Add(std::string_view ikey, std::string_view value) {
  if (num_entries_ == 0) smallest_key_.assign(ikey);
  std::string_view user_key = ExtractUserKey(ikey);
  // The filter is over user keys; consecutive versions of one key are added once.
  if (use_bloom_ && (num_entries_ == 0 || user_key != last_user_key_)) {
    bloom_.AddKey(user_key);
    last_user_key_.assign(user_key);
  }
  data_block_.Add(ikey, value);
  last_key_.assign(ikey);
  num_entries_++;
  if (data_block_.CurrentSize() >= block_size_) FlushDataBlock();
}

void TableBuilder::WriteBlockWithCrc(std::string_view contents) {
  char crc[kBlockTrailerSize];
  EncodeFixed32(crc, crc32c::Value(contents));
  if (status_.ok()) status_ = file_->Append(contents);
  if (status_.ok()) status_ = file_->Append(std::string_view(crc, kBlockTrailerSize));
  offset_ += contents.size() + kBlockTrailerSize;
}

void TableBuilder::FlushDataBlock() {
  if (data_block_.empty()) return;
  std::string contents = data_block_.Finish();
  // The index maps the block's last key to its location; a lookup binary-searches it.
  PutLengthPrefixed(&index_, last_key_);
  PutFixed64(&index_, offset_);
  PutFixed32(&index_, static_cast<uint32_t>(contents.size()));
  WriteBlockWithCrc(contents);
}

Status TableBuilder::Finish() {
  FlushDataBlock();
  const uint64_t filter_offset = offset_;
  std::string filter = use_bloom_ ? bloom_.Finish() : std::string();
  WriteBlockWithCrc(filter);
  const uint64_t index_offset = offset_;
  WriteBlockWithCrc(index_);

  std::string footer;
  PutFixed64(&footer, index_offset);
  PutFixed64(&footer, index_.size());
  PutFixed64(&footer, filter_offset);
  PutFixed64(&footer, filter.size());
  PutFixed32(&footer, kTableFormatVersion);
  PutFixed32(&footer, crc32c::Value(footer));
  PutFixed64(&footer, kTableMagic);
  if (status_.ok()) status_ = file_->Append(footer);
  offset_ += footer.size();
  if (status_.ok()) status_ = file_->Flush();
  if (status_.ok()) status_ = file_->Sync();
  if (status_.ok()) status_ = file_->Close();
  return status_;
}

}  // namespace docstore::kv
