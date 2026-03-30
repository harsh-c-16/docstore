#include "kv/block_cache.h"

#include "util/coding.h"
#include "util/hash.h"

namespace docstore::kv {

size_t BlockCache::KeyHash::operator()(const Key& k) const {
  char buf[16];
  EncodeFixed64(buf, k.file_number);
  EncodeFixed64(buf + 8, k.offset);
  return static_cast<size_t>(Hash64(std::string_view(buf, 16)));
}

int BlockCache::ShardFor(uint64_t file_number, uint64_t offset) {
  // Top bits of the hash pick the shard; the low bits are used inside the shard's hash map.
  return static_cast<int>(KeyHash()(Key{file_number, offset}) >> 60);
}

BlockCache::BlockCache(size_t capacity_bytes) : shard_capacity_(capacity_bytes / kNumShards) {}

std::shared_ptr<const Block> BlockCache::Lookup(uint64_t file_number, uint64_t offset) {
  Shard& shard = shards_[ShardFor(file_number, offset)];
  std::lock_guard<std::mutex> l(shard.mu);
  auto it = shard.index.find(Key{file_number, offset});
  if (it == shard.index.end()) {
    misses_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  shard.lru.splice(shard.lru.begin(), shard.lru, it->second);  // mark most recently used
  hits_.fetch_add(1, std::memory_order_relaxed);
  return it->second->block;
}

void BlockCache::Insert(uint64_t file_number, uint64_t offset, std::shared_ptr<const Block> block) {
  const size_t charge = block->size();
  if (charge > shard_capacity_) return;
  Key key{file_number, offset};
  Shard& shard = shards_[ShardFor(file_number, offset)];
  std::lock_guard<std::mutex> l(shard.mu);
  if (auto it = shard.index.find(key); it != shard.index.end()) {
    shard.usage -= it->second->block->size();
    shard.lru.erase(it->second);
    shard.index.erase(it);
  }
  shard.lru.push_front(Entry{key, std::move(block)});
  shard.index[key] = shard.lru.begin();
  shard.usage += charge;
  while (shard.usage > shard_capacity_) {
    Entry& victim = shard.lru.back();
    shard.usage -= victim.block->size();
    shard.index.erase(victim.key);
    shard.lru.pop_back();
    evictions_.fetch_add(1, std::memory_order_relaxed);
  }
}

size_t BlockCache::usage() const {
  size_t total = 0;
  for (const Shard& shard : shards_) {
    std::lock_guard<std::mutex> l(shard.mu);
    total += shard.usage;
  }
  return total;
}

}  // namespace docstore::kv
