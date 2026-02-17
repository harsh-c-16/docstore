#pragma once

// A VersionEdit is one change to the set of live files: the unit written to the MANIFEST.
// Encoding: a sequence of tagged fields, tag varint32 then
//   1 log_number varint64 | 2 next_file_number varint64 | 3 last_sequence varint64 |
//   4 add_file: level varint32, number varint64, file_size varint64,
//               smallest (length-prefixed internal key), largest (length-prefixed) |
//   5 delete_file: level varint32, number varint64

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "docstore/status.h"
#include "kv/dbformat.h"

namespace docstore::kv {

constexpr int kNumLevels = 2;  // L0 (overlapping, from flushes) and L1 (sorted, from compaction)

class TableFile;

struct FileMeta {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // internal keys
  std::string largest;
  // The open table. Set when the file is installed into a Version; owning it keeps the file
  // on disk until the last Version that lists it is released.
  std::shared_ptr<TableFile> table;
};

class VersionEdit {
 public:
  void SetLogNumber(uint64_t n) { log_number = n; }
  void SetNextFileNumber(uint64_t n) { next_file_number = n; }
  void SetLastSequence(SequenceNumber s) { last_sequence = s; }
  void AddFile(int level, FileMeta meta) { new_files.emplace_back(level, std::move(meta)); }
  void DeleteFile(int level, uint64_t number) { deleted_files.emplace_back(level, number); }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view src);

  std::optional<uint64_t> log_number;
  std::optional<uint64_t> next_file_number;
  std::optional<SequenceNumber> last_sequence;
  std::vector<std::pair<int, FileMeta>> new_files;
  std::vector<std::pair<int, uint64_t>> deleted_files;
};

}  // namespace docstore::kv
