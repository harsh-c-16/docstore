#pragma once

// In-memory write buffer: a skiplist of internal keys whose memory lives in an arena.
// Entry layout in the arena: varint ikey_len | internal key | varint val_len | value.
// One writer (under the engine's write mutex), any number of concurrent readers. Memtables are
// owned by shared_ptr so a reader or iterator keeps one alive after it has been flushed.

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "kv/dbformat.h"
#include "kv/iterator.h"
#include "kv/skiplist.h"
#include "util/arena.h"

namespace docstore::kv {

class MemTable {
 public:
  MemTable();
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  void Add(SequenceNumber seq, ValueType type, std::string_view user_key, std::string_view value);

  enum class LookupResult { kNotPresent, kFound, kDeleted };
  // Finds the newest version of user_key with sequence <= snapshot.
  LookupResult Get(std::string_view user_key, SequenceNumber snapshot, std::string* value) const;

  // Iterates internal keys in order. The iterator must not outlive the memtable.
  std::unique_ptr<Iterator> NewIterator() const;

  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }
  uint64_t num_entries() const { return num_entries_.load(std::memory_order_relaxed); }

 private:
  struct KeyComparator {
    // Entries are compared by their length-prefixed internal keys.
    int operator()(const char* a, const char* b) const;
  };
  using Table = SkipList<const char*, KeyComparator>;
  friend class MemTableIterator;

  Arena arena_;
  Table table_;
  std::atomic<uint64_t> num_entries_{0};
};

// Decodes the length-prefixed internal key at the start of a memtable entry.
std::string_view GetLengthPrefixedKey(const char* entry);

}  // namespace docstore::kv
