#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "util/arena.h"
#include "util/random.h"

namespace docstore {
namespace {

TEST(ArenaTest, AllocationsAreDistinctAndWritable) {
  Arena arena;
  Random rnd(301);
  std::vector<std::pair<char*, size_t>> allocs;
  size_t total = 0;
  for (int i = 0; i < 2000; i++) {
    size_t n = (i % 50 == 0) ? 3000 : 1 + rnd.Uniform(100);  // mix of small and large
    char* p = (i % 2) ? arena.AllocateAligned(n) : arena.Allocate(n);
    if (i % 2) {
      EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t), 0u);
    }
    std::memset(p, i & 0xFF, n);
    allocs.emplace_back(p, n);
    total += n;
    EXPECT_GE(arena.MemoryUsage(), total);
  }
  // Every allocation still holds its own fill byte: nothing overlapped.
  for (size_t i = 0; i < allocs.size(); i++) {
    for (size_t j = 0; j < allocs[i].second; j++) {
      ASSERT_EQ(static_cast<uint8_t>(allocs[i].first[j]), i & 0xFF);
    }
  }
  EXPECT_LT(arena.MemoryUsage(), total * 2 + 64 * Arena::kBlockSize);
}

TEST(RandomTest, DeterministicAndInRange) {
  Random a(42), b(42), c(43);
  bool differs = false;
  for (int i = 0; i < 1000; i++) {
    uint64_t x = a.Next();
    EXPECT_EQ(x, b.Next());
    differs |= (x != c.Next());
  }
  for (int i = 0; i < 1000; i++) EXPECT_LT(a.Uniform(10), 10u);
  EXPECT_TRUE(differs);
  Random zero(0);  // SplitMix64 has no bad seed
  std::set<uint64_t> seen;
  for (int i = 0; i < 100; i++) seen.insert(zero.Next());
  EXPECT_EQ(seen.size(), 100u);
}

}  // namespace
}  // namespace docstore
