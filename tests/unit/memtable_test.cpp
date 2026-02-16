#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "kv/dbformat.h"
#include "kv/memtable.h"
#include "kv/skiplist.h"
#include "util/arena.h"
#include "util/coding.h"
#include "util/random.h"

namespace docstore::kv {
namespace {

std::string IKey(std::string_view user_key, SequenceNumber seq, ValueType t) {
  std::string k;
  AppendInternalKey(&k, user_key, seq, t);
  return k;
}

TEST(DbFormatTest, ParseRoundTrip) {
  ParsedInternalKey p;
  // p.user_key points into the key, so the key must outlive p.
  const std::string key = IKey("user", 12345, ValueType::kDeletion);
  ASSERT_TRUE(ParseInternalKey(key, &p));
  EXPECT_EQ(p.user_key, "user");
  EXPECT_EQ(p.sequence, 12345u);
  EXPECT_EQ(p.type, ValueType::kDeletion);
  const std::string max_key = IKey("", kMaxSequenceNumber, ValueType::kValue);
  ASSERT_TRUE(ParseInternalKey(max_key, &p));
  EXPECT_EQ(p.sequence, kMaxSequenceNumber);
  EXPECT_FALSE(ParseInternalKey("short", &p));
  std::string bad = IKey("k", 1, ValueType::kValue);
  bad[bad.size() - 8] = 7;  // unknown type byte
  EXPECT_FALSE(ParseInternalKey(bad, &p));
}

TEST(DbFormatTest, OrderIsUserKeyAscThenSequenceDesc) {
  std::vector<std::string> sorted = {
      IKey("a", 100, ValueType::kValue), IKey("a", 100, ValueType::kDeletion),
      IKey("a", 5, ValueType::kValue),   IKey("ab", 1, ValueType::kValue),
      IKey("b", 300, ValueType::kValue), IKey("b", 2, ValueType::kDeletion),
  };
  for (size_t i = 0; i + 1 < sorted.size(); i++) {
    EXPECT_LT(CompareInternalKeys(sorted[i], sorted[i + 1]), 0) << i;
    EXPECT_GT(CompareInternalKeys(sorted[i + 1], sorted[i]), 0) << i;
  }
  EXPECT_EQ(CompareInternalKeys(sorted[0], sorted[0]), 0);
  // A lookup at snapshot 50 sorts after the version at 100 and before the version at 5.
  std::string lookup = MakeLookupKey("a", 50);
  EXPECT_GT(CompareInternalKeys(lookup, sorted[1]), 0);
  EXPECT_LT(CompareInternalKeys(lookup, sorted[2]), 0);
}

struct U64Comparator {
  int operator()(uint64_t a, uint64_t b) const { return a < b ? -1 : (a > b ? 1 : 0); }
};
using IntSkipList = SkipList<uint64_t, U64Comparator>;

TEST(SkipListTest, InsertContainsIterateSeek) {
  Arena arena;
  IntSkipList list(U64Comparator(), &arena);
  Random rnd(7);
  std::set<uint64_t> model;
  for (int i = 0; i < 5000; i++) {
    uint64_t k = rnd.Uniform(100000);
    if (model.insert(k).second) list.Insert(k);
  }
  for (uint64_t k = 0; k < 100000; k += 37) EXPECT_EQ(list.Contains(k), model.count(k) == 1);

  IntSkipList::Iterator it(&list);
  it.SeekToFirst();
  for (uint64_t k : model) {
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.key(), k);
    it.Next();
  }
  EXPECT_FALSE(it.Valid());

  for (uint64_t target : {uint64_t{0}, uint64_t{500}, uint64_t{99999}, uint64_t{100001}}) {
    it.Seek(target);
    auto m = model.lower_bound(target);
    if (m == model.end()) {
      EXPECT_FALSE(it.Valid());
    } else {
      ASSERT_TRUE(it.Valid());
      EXPECT_EQ(it.key(), *m);
    }
  }
}

// One writer inserts a known random permutation while 4 readers iterate. Every reader must see
// a sorted sequence that contains at least every key published before its pass started.
// Run under TSan to check the acquire/release protocol.
TEST(SkipListTest, ConcurrentOneWriterFourReaders) {
  constexpr int kKeys = 20000;
  Arena arena;
  IntSkipList list(U64Comparator(), &arena);
  std::vector<uint64_t> keys(kKeys);
  for (int i = 0; i < kKeys; i++) keys[i] = static_cast<uint64_t>(i) * 2 + 1;  // odd keys only
  Random shuffle_rnd(99);
  for (int i = kKeys - 1; i > 0; i--) std::swap(keys[i], keys[shuffle_rnd.Uniform(i + 1)]);

  std::atomic<int> published{0};
  std::atomic<bool> failed{false};
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; r++) {
    readers.emplace_back([&] {
      while (published.load(std::memory_order_acquire) < kKeys && !failed) {
        int before = published.load(std::memory_order_acquire);
        std::set<uint64_t> seen;
        uint64_t last = 0;
        IntSkipList::Iterator it(&list);
        for (it.SeekToFirst(); it.Valid(); it.Next()) {
          if (it.key() <= last || it.key() % 2 == 0) failed = true;  // unsorted or garbage
          last = it.key();
          seen.insert(it.key());
        }
        for (int i = 0; i < before; i++) {
          if (!seen.count(keys[i])) failed = true;  // a published key went missing
        }
      }
    });
  }
  for (int i = 0; i < kKeys; i++) {
    list.Insert(keys[i]);
    published.store(i + 1, std::memory_order_release);
  }
  for (auto& t : readers) t.join();
  EXPECT_FALSE(failed);
}

TEST(MemTableTest, OverwriteDeleteAndSnapshots) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "k", "v1");
  mem.Add(2, ValueType::kValue, "k", "v2");
  mem.Add(3, ValueType::kDeletion, "k", "");
  mem.Add(4, ValueType::kValue, "other", "x");
  std::string v;
  EXPECT_EQ(mem.Get("k", 1, &v), MemTable::LookupResult::kFound);
  EXPECT_EQ(v, "v1");
  EXPECT_EQ(mem.Get("k", 2, &v), MemTable::LookupResult::kFound);
  EXPECT_EQ(v, "v2");
  EXPECT_EQ(mem.Get("k", 3, &v), MemTable::LookupResult::kDeleted);
  EXPECT_EQ(mem.Get("k", 100, &v), MemTable::LookupResult::kDeleted);
  EXPECT_EQ(mem.Get("k", 0, &v), MemTable::LookupResult::kNotPresent);
  EXPECT_EQ(mem.Get("missing", 100, &v), MemTable::LookupResult::kNotPresent);
  EXPECT_EQ(mem.Get("o", 100, &v), MemTable::LookupResult::kNotPresent);  // prefix of "other"
  EXPECT_EQ(mem.num_entries(), 4u);
  EXPECT_GT(mem.ApproximateMemoryUsage(), 0u);
}

TEST(MemTableTest, IteratorYieldsInternalKeyOrder) {
  MemTable mem;
  Random rnd(5);
  std::vector<std::string> expected;
  SequenceNumber seq = 1;
  for (int i = 0; i < 1000; i++) {
    std::string key = "key" + std::to_string(rnd.Uniform(200));
    std::string value(rnd.Uniform(20), 'v');
    ValueType t = rnd.OneIn(5) ? ValueType::kDeletion : ValueType::kValue;
    mem.Add(seq, t, key, t == ValueType::kValue ? value : "");
    expected.push_back(IKey(key, seq, t));
    seq++;
  }
  std::sort(expected.begin(), expected.end(), [](const std::string& a, const std::string& b) {
    return CompareInternalKeys(a, b) < 0;
  });
  auto it = mem.NewIterator();
  size_t i = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), i++) {
    ASSERT_LT(i, expected.size());
    EXPECT_EQ(it->key(), expected[i]);
  }
  EXPECT_EQ(i, expected.size());
  it->Seek(MakeLookupKey("key150", kMaxSequenceNumber));
  ASSERT_TRUE(it->Valid());
  EXPECT_GE(ExtractUserKey(it->key()), "key150");
  EXPECT_TRUE(it->status().ok());
}

TEST(MemTableTest, LargeValuesAndEmptyKey) {
  MemTable mem;
  std::string big(100000, 'b');
  mem.Add(1, ValueType::kValue, "", big);
  mem.Add(2, ValueType::kValue, std::string_view("\0k", 2), "nul");
  std::string v;
  ASSERT_EQ(mem.Get("", 10, &v), MemTable::LookupResult::kFound);
  EXPECT_EQ(v, big);
  ASSERT_EQ(mem.Get(std::string_view("\0k", 2), 10, &v), MemTable::LookupResult::kFound);
  EXPECT_EQ(v, "nul");
}

}  // namespace
}  // namespace docstore::kv
