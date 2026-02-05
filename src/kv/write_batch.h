#pragma once

// A group of puts/deletes applied atomically: the unit of the WAL and of recovery.
// Encoding: seq u64 | count u32 | records..., each record
//   type u8 | varint key_len | key | (Put only) varint val_len | value

#include <cstdint>
#include <string>
#include <string_view>

#include "docstore/status.h"
#include "kv/dbformat.h"

namespace docstore::kv {

class MemTable;

class WriteBatch {
 public:
  static constexpr size_t kHeaderSize = 12;

  WriteBatch() { Clear(); }

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();

  uint32_t Count() const;
  SequenceNumber Sequence() const;
  void SetSequence(SequenceNumber seq);

  std::string_view Contents() const { return rep_; }
  size_t ApproximateSize() const { return rep_.size(); }
  // Adopts an encoded batch (read back from the WAL). Only the header length is checked here;
  // Iterate() validates every record.
  Status SetContents(std::string_view contents);

  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(std::string_view key, std::string_view value) = 0;
    virtual void Delete(std::string_view key) = 0;
  };
  // Corruption if a record is malformed or the record count does not match the header.
  Status Iterate(Handler* handler) const;

 private:
  std::string rep_;
};

// Applies the batch to the memtable; record i gets sequence Sequence() + i.
Status InsertInto(const WriteBatch& batch, MemTable* mem);

}  // namespace docstore::kv
