#pragma once

// Read side of an SSTable. Open() validates the footer and loads the index and filter blocks
// into memory; Get() costs at most one data-block read (often served by the block cache).
// Thread-safe: any number of readers can use one TableReader at once.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "docstore/options.h"
#include "docstore/status.h"
#include "env/env.h"
#include "kv/block.h"
#include "kv/dbformat.h"
#include "kv/iterator.h"

namespace docstore::kv {

class BlockCache;

class TableReader {
 public:
  static Status Open(const Options& options, std::unique_ptr<RandomAccessFile> file,
                     uint64_t file_size, uint64_t file_number, BlockCache* cache,
                     std::unique_ptr<TableReader>* out);

  bool has_filter() const { return !filter_.empty(); }
  // False only if the key is definitely not in this table.
  bool KeyMayMatch(std::string_view user_key) const;

  enum class GetResult { kNotPresent, kFound, kDeleted };
  // Newest version of user_key with sequence <= snapshot (lookup_key = MakeLookupKey(...)).
  Status Get(std::string_view lookup_key, GetResult* result, std::string* value) const;

  // Iterator over internal keys. The TableReader must outlive it.
  std::unique_ptr<Iterator> NewIterator() const;

  uint64_t file_number() const { return file_number_; }
  size_t num_blocks() const { return index_.size(); }

 private:
  friend class TableIterator;
  struct IndexEntry {
    std::string last_key;
    uint64_t offset;
    uint32_t size;
  };

  TableReader() = default;
  // Index of the first block whose last key >= ikey, or num_blocks() if none.
  size_t FindBlock(std::string_view ikey) const;
  Status ReadBlock(size_t i, std::shared_ptr<const Block>* out) const;

  bool paranoid_checks_ = true;
  std::unique_ptr<RandomAccessFile> file_;
  uint64_t file_number_ = 0;
  BlockCache* cache_ = nullptr;
  std::vector<IndexEntry> index_;
  std::string filter_;
};

}  // namespace docstore::kv
