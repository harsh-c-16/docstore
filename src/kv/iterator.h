#pragma once

// Forward iterator over sorted key/value pairs. Internal iterators (memtable, table, merging)
// yield internal keys; the DB iterator yields user keys. key()/value() stay valid until the
// iterator moves.

#include <string_view>

#include "docstore/status.h"

namespace docstore::kv {

class Iterator {
 public:
  virtual ~Iterator() = default;
  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  // Positions at the first entry with key >= target.
  virtual void Seek(std::string_view target) = 0;
  virtual void Next() = 0;
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
  // Non-OK if the iterator stopped early because of corruption or an I/O error.
  virtual Status status() const = 0;
};

}  // namespace docstore::kv
