#pragma once

// Bump allocator for the memtable. Many tiny allocations (skiplist nodes + entries) become
// a handful of 4 KiB blocks, and everything is freed at once when the memtable is destroyed.
// Not thread-safe for allocation: only the single writer allocates (see DESIGN.md, D7).

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace docstore {

class Arena {
 public:
  static constexpr size_t kBlockSize = 4096;

  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  char* Allocate(size_t bytes);
  // Aligned for any type (alignof(std::max_align_t)); skiplist nodes hold atomics.
  char* AllocateAligned(size_t bytes);

  // Bytes handed out to callers (including alignment padding). This, not the reserved block
  // memory, decides when a memtable is full, so even a tiny test memtable holds many entries.
  // Safe to read from any thread.
  size_t MemoryUsage() const { return memory_usage_.load(std::memory_order_relaxed); }

 private:
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t bytes);

  char* alloc_ptr_ = nullptr;
  size_t alloc_remaining_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

}  // namespace docstore
