#pragma once

// Sharded LRU cache of parsed data blocks, keyed by (file number, block offset). Capacity is
// in bytes, split evenly over 16 shards picked by hash, each with its own mutex, so readers on
// different blocks rarely contend. Values are shared_ptr<const Block>: an evicted block stays
// valid for any reader still using it. File numbers are never reused, so entries of a deleted
// file can never be confused with a new file; they simply age out.

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "kv/block.h"

namespace docstore::kv {

class BlockCache {
 public:
  static constexpr int kNumShards = 16;

  explicit BlockCache(size_t capacity_bytes);

  // nullptr on a miss.
  std::shared_ptr<const Block> Lookup(uint64_t file_number, uint64_t offset);
  // Inserts (or replaces) and evicts least-recently-used blocks until the shard fits.
  // A block larger than a whole shard is not cached.
  void Insert(uint64_t file_number, uint64_t offset, std::shared_ptr<const Block> block);

  uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }
  uint64_t misses() const { return misses_.load(std::memory_order_relaxed); }
  uint64_t evictions() const { return evictions_.load(std::memory_order_relaxed); }
  size_t usage() const;
  static int ShardFor(uint64_t file_number, uint64_t offset);

 private:
  struct Key {
    uint64_t file_number;
    uint64_t offset;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    size_t operator()(const Key& k) const;
  };
  struct Entry {
    Key key;
    std::shared_ptr<const Block> block;
  };
  struct Shard {
    mutable std::mutex mu;
    std::list<Entry> lru;  // front = most recently used
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index;
    size_t usage = 0;
  };

  const size_t shard_capacity_;
  Shard shards_[kNumShards];
  std::atomic<uint64_t> hits_{0}, misses_{0}, evictions_{0};
};

}  // namespace docstore::kv
