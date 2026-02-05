#pragma once

// Parses a record log that has been read fully into memory. WAL files are bounded by the
// memtable size plus one batch and MANIFESTs are small, so reading whole files keeps the parser
// simple (DESIGN.md D10).

#include <cstdint>
#include <string>
#include <string_view>

#include "docstore/status.h"

namespace docstore::kv {

class LogReader {
 public:
  enum class Problem {
    kNone,        // clean end of file
    kIncomplete,  // header or payload cut off by end of file
    kChecksum,    // complete record whose CRC does not match
    kOversized,   // length field above kMaxLogRecordSize
  };

  explicit LogReader(std::string_view contents) : data_(contents) {}

  // Returns the next valid record. Returns false at end of file or at the first bad record;
  // problem() then says which, and nothing after a bad record is ever returned.
  bool ReadRecord(std::string_view* record);

  Problem problem() const { return problem_; }
  // Offset just past the last good record: where a torn file would be cut.
  uint64_t valid_prefix_length() const { return valid_end_; }
  // OK at a clean end, Corruption describing the bad record otherwise.
  Status status() const;

  // True if the bad record looks like an interrupted append rather than damage: it was cut off
  // by end of file, its stored CRC does not match if we assume the length field was damaged,
  // and no valid record exists anywhere after it. Used for the MANIFEST (DESIGN.md D11).
  bool BadRecordIsTornTail() const;

 private:
  bool ValidRecordAt(size_t offset, size_t* end) const;

  std::string_view data_;
  size_t pos_ = 0;
  uint64_t valid_end_ = 0;
  Problem problem_ = Problem::kNone;
};

}  // namespace docstore::kv
