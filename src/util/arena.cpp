#include "util/arena.h"

#include <cstdint>

namespace docstore {

char* Arena::Allocate(size_t bytes) {
  if (bytes <= alloc_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

char* Arena::AllocateAligned(size_t bytes) {
  constexpr size_t kAlign = alignof(std::max_align_t);
  size_t mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (kAlign - 1);
  size_t slop = mod == 0 ? 0 : kAlign - mod;
  if (bytes + slop <= alloc_remaining_) {
    char* result = alloc_ptr_ + slop;
    alloc_ptr_ += bytes + slop;
    alloc_remaining_ -= bytes + slop;
    return result;
  }
  // New blocks come from operator new[], which is already max-aligned.
  return AllocateFallback(bytes);
}

char* Arena::AllocateFallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // Large object: give it its own block so the rest of the current block is not wasted.
    return AllocateNewBlock(bytes);
  }
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_remaining_ = kBlockSize;
  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateNewBlock(size_t bytes) {
  blocks_.push_back(std::make_unique_for_overwrite<char[]>(bytes));
  memory_usage_.fetch_add(bytes + sizeof(char*), std::memory_order_relaxed);
  return blocks_.back().get();
}

}  // namespace docstore
