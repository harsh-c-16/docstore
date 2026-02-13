#pragma once

// Data block of an SSTable: entries `varint klen | varint vlen | internal key | value`, sorted by
// internal key, followed on disk by a crc32c of the entries. There are no restart points (prefix
// compression is future work), so a seek scans the block from the start; blocks are ~4 KiB.

#include <memory>
#include <string>
#include <string_view>

#include "kv/iterator.h"

namespace docstore::kv {

class BlockBuilder {
 public:
  void Add(std::string_view key, std::string_view value);
  size_t CurrentSize() const { return buffer_.size(); }
  bool empty() const { return buffer_.empty(); }
  // Returns the block contents (without the CRC) and resets the builder.
  std::string Finish();

 private:
  std::string buffer_;
};

// An immutable parsed block. Shared through the block cache as shared_ptr<const Block>.
class Block {
 public:
  explicit Block(std::string contents) : data_(std::move(contents)) {}
  size_t size() const { return data_.size(); }
  // Iterator over internal keys. Holds a reference to the block, so it may outlive the cache
  // entry. Malformed entries end iteration with a Corruption status.
  static std::unique_ptr<Iterator> NewIterator(std::shared_ptr<const Block> block);

  const std::string& data() const { return data_; }

 private:
  std::string data_;
};

}  // namespace docstore::kv
