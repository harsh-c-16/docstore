#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace docstore {

class Env;

// Lets compaction discard keys that are dead for reasons the engine cannot see (the document
// layer drops whole collections and indexes this way). Contract: ShouldDrop may only return
// true for keys that can never be written again. Called from the background thread.
class CompactionFilter {
 public:
  virtual ~CompactionFilter() = default;
  virtual bool ShouldDrop(std::string_view user_key) const = 0;
};

struct Options {
  bool create_if_missing = true;
  bool error_if_exists = false;
  size_t memtable_size = 4 << 20;
  int l0_compaction_trigger = 4;
  int l0_slowdown_trigger = 8;
  int l0_stop_trigger = 12;
  uint64_t target_file_size = 8 << 20;
  size_t block_size = 4 << 10;
  int bloom_bits_per_key = 10;         // 0 disables bloom filters
  size_t block_cache_bytes = 8 << 20;  // 0 disables the block cache
  // Background WAL sync period for sync=false writes (MongoDB's commitIntervalMs is 100).
  // 0 disables background syncing.
  uint32_t journal_commit_interval_ms = 100;
  bool paranoid_checks = true;  // verify checksums on every read
  Env* env = nullptr;           // nullptr means the POSIX filesystem
  std::shared_ptr<const CompactionFilter> compaction_filter;
  // Test-only: called with a name at fixed points inside flush, compaction and index builds,
  // so tests can inject a crash there. Must not call back into the database.
  std::function<void(std::string_view point)> test_hook;
};

// sync=true is like MongoDB's write concern j:true: the WAL is fdatasync'd before returning.
struct WriteOptions {
  bool sync = false;
};

}  // namespace docstore
