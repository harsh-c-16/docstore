#pragma once

// Writes one SSTable from entries added in internal-key order.

#include <memory>
#include <string>
#include <string_view>

#include "docstore/options.h"
#include "docstore/status.h"
#include "env/env.h"
#include "kv/block.h"
#include "kv/bloom.h"

namespace docstore::kv {

class TableBuilder {
 public:
  TableBuilder(const Options& options, std::unique_ptr<WritableFile> file);

  // Precondition: ikey sorts after every key added before.
  void Add(std::string_view ikey, std::string_view value);
  // Writes the filter, index and footer, then Flush + Sync + Close: the contents are durable
  // on return (the caller syncs the directory).
  Status Finish();

  uint64_t num_entries() const { return num_entries_; }
  // Bytes written so far; equals the final file size after Finish().
  uint64_t FileSize() const { return offset_ + data_block_.CurrentSize(); }
  const std::string& smallest_key() const { return smallest_key_; }
  const std::string& largest_key() const { return last_key_; }

 private:
  void FlushDataBlock();
  void WriteBlockWithCrc(std::string_view contents);

  const size_t block_size_;
  const bool use_bloom_;
  std::unique_ptr<WritableFile> file_;
  uint64_t offset_ = 0;
  BlockBuilder data_block_;
  std::string index_;
  BloomFilterBuilder bloom_;
  std::string smallest_key_;
  std::string last_key_;
  std::string last_user_key_;
  uint64_t num_entries_ = 0;
  Status status_;
};

}  // namespace docstore::kv
