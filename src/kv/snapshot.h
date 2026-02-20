#pragma once

// A consistent read view: the memtables, the Version and the sequence number, captured
// together under the engine mutex. Holding a Snapshot keeps all of them alive (memtables via
// shared_ptr, table files via the Version), so every Get and iterator created from it sees the
// same state no matter how many writes, flushes or compactions happen meanwhile.
// A Snapshot must not outlive the Engine that created it.

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "docstore/status.h"
#include "kv/dbformat.h"
#include "kv/iterator.h"

namespace docstore::kv {

class MemTable;
struct Version;

// Range of user keys [lower, upper). An empty upper bound means "to the end".
struct ReadOptions {
  std::string lower;
  std::string upper;
};

struct ReadStats {
  std::atomic<uint64_t> bloom_checks{0};
  std::atomic<uint64_t> bloom_useful{0};  // reads skipped because the filter said "absent"
};

class Snapshot : public std::enable_shared_from_this<Snapshot> {
 public:
  Snapshot(std::shared_ptr<MemTable> mem, std::shared_ptr<MemTable> imm,
           std::shared_ptr<const Version> version, SequenceNumber sequence, ReadStats* stats)
      : mem_(std::move(mem)),
        imm_(std::move(imm)),
        version_(std::move(version)),
        sequence_(sequence),
        stats_(stats) {}

  // NotFound if the key is absent or deleted at this snapshot.
  Status Get(std::string_view key, std::string* value) const;
  // User-key iterator over [lower, upper). Keeps this snapshot alive.
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) const;

  SequenceNumber sequence() const { return sequence_; }

 private:
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;  // may be null
  std::shared_ptr<const Version> version_;
  SequenceNumber sequence_;
  ReadStats* stats_;
};

}  // namespace docstore::kv
