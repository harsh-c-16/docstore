#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <memory>

#include "env/mem_env.h"
#include "kv/block_cache.h"
#include "kv/bloom.h"
#include "kv/dbformat.h"
#include "kv/table_builder.h"
#include "kv/table_format.h"
#include "kv/table_reader.h"
#include "util/random.h"

namespace docstore::kv {
namespace {

std::string IKey(std::string_view k, SequenceNumber s, ValueType t = ValueType::kValue) {
  std::string out;
  AppendInternalKey(&out, k, s, t);
  return out;
}

std::string Key(int i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "key%08d", i);
  return buf;
}

class TableTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(env_.CreateDir("/db").ok()); }

  // Builds a table from entries already in internal-key order.
  void Build(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(env_.NewWritableFile(path_, &file).ok());
    TableBuilder builder(options_, std::move(file));
    for (const auto& [k, v] : entries) builder.Add(k, v);
    ASSERT_TRUE(builder.Finish().ok());
    uint64_t size;
    ASSERT_TRUE(env_.GetFileSize(path_, &size).ok());
    EXPECT_EQ(size, builder.FileSize());
  }

  Status OpenTable(std::unique_ptr<TableReader>* out, BlockCache* cache = nullptr) {
    std::unique_ptr<RandomAccessFile> file;
    Status s = env_.NewRandomAccessFile(path_, &file);
    if (!s.ok()) return s;
    uint64_t size;
    s = env_.GetFileSize(path_, &size);
    if (!s.ok()) return s;
    return TableReader::Open(options_, std::move(file), size, 7, cache, out);
  }

  void FlipByte(uint64_t offset) {
    std::string contents;
    ASSERT_TRUE(ReadFileToString(&env_, path_, &contents).ok());
    contents[offset] ^= 0x5A;
    ASSERT_TRUE(WriteStringToFileSync(&env_, contents, path_).ok());
  }

  uint64_t FileSize() {
    uint64_t size;
    EXPECT_TRUE(env_.GetFileSize(path_, &size).ok());
    return size;
  }

  MemEnv env_;
  Options options_;
  std::string path_ = "/db/000007.sst";
};

TEST_F(TableTest, RoundTrip100KKeysWithVersions) {
  std::vector<std::pair<std::string, std::string>> entries;
  std::map<std::string, std::pair<SequenceNumber, std::string>> newest;  // per user key
  Random rnd(11);
  for (int i = 0; i < 100000; i++) {
    std::string k = Key(i * 2);  // even keys only, so odd keys are absent
    int versions = 1 + static_cast<int>(rnd.Uniform(3));
    for (int v = versions; v >= 1; v--) {  // newest (highest seq) first
      SequenceNumber seq = static_cast<SequenceNumber>(i) * 10 + v;
      bool del = v == versions && rnd.OneIn(10);
      entries.emplace_back(IKey(k, seq, del ? ValueType::kDeletion : ValueType::kValue),
                           del ? "" : "val" + std::to_string(seq));
      if (v == versions) newest[k] = {seq, del ? "" : "val" + std::to_string(seq)};
    }
  }
  Build(entries);
  std::unique_ptr<TableReader> table;
  ASSERT_TRUE(OpenTable(&table).ok());
  EXPECT_GT(table->num_blocks(), 100u);

  auto it = table->NewIterator();
  size_t n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), n++) {
    ASSERT_EQ(it->key(), entries[n].first);
    ASSERT_EQ(it->value(), entries[n].second);
  }
  EXPECT_EQ(n, entries.size());
  EXPECT_TRUE(it->status().ok());

  for (int trial = 0; trial < 20000; trial++) {
    int i = static_cast<int>(rnd.Uniform(200000));
    std::string k = Key(i);
    TableReader::GetResult result;
    std::string value;
    ASSERT_TRUE(table->Get(MakeLookupKey(k, kMaxSequenceNumber), &result, &value).ok());
    auto m = newest.find(k);
    if (m == newest.end()) {
      ASSERT_EQ(result, TableReader::GetResult::kNotPresent) << k;
    } else if (m->second.second.empty()) {
      ASSERT_EQ(result, TableReader::GetResult::kDeleted) << k;
    } else {
      ASSERT_EQ(result, TableReader::GetResult::kFound) << k;
      ASSERT_EQ(value, m->second.second);
      ASSERT_TRUE(table->KeyMayMatch(k));  // never a false negative
    }
  }
  // A snapshot older than every version of the key sees nothing.
  TableReader::GetResult result;
  std::string value;
  ASSERT_TRUE(table->Get(MakeLookupKey(Key(10), 1), &result, &value).ok());
  EXPECT_EQ(result, TableReader::GetResult::kNotPresent);

  it->Seek(IKey(Key(1001), kMaxSequenceNumber));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractUserKey(it->key()), Key(1002));
}

TEST_F(TableTest, CorruptedDataBlockIsReportedNotReturned) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 2000; i++) entries.emplace_back(IKey(Key(i), 1), std::string(100, 'v'));
  Build(entries);
  FlipByte(10);  // inside the first data block
  std::unique_ptr<TableReader> table;
  ASSERT_TRUE(OpenTable(&table).ok());  // footer, index and filter are intact
  TableReader::GetResult result;
  std::string value;
  EXPECT_TRUE(table->Get(MakeLookupKey(Key(0), 5), &result, &value).IsCorruption());
  auto it = table->NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
  // Blocks far from the damage still read fine.
  ASSERT_TRUE(table->Get(MakeLookupKey(Key(1999), 5), &result, &value).ok());
  EXPECT_EQ(result, TableReader::GetResult::kFound);
}

TEST_F(TableTest, CorruptedMetadataFailsOpen) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 500; i++) entries.emplace_back(IKey(Key(i), 1), "value");
  Build(entries);
  const uint64_t size = FileSize();
  std::string pristine;
  ASSERT_TRUE(ReadFileToString(&env_, path_, &pristine).ok());
  // Footer magic, footer fields, the last index bytes, and the filter's k byte region.
  for (uint64_t offset : {size - 1, size - kFooterSize, size - kFooterSize - 6, size - 30}) {
    ASSERT_TRUE(WriteStringToFileSync(&env_, pristine, path_).ok());
    FlipByte(offset);
    std::unique_ptr<TableReader> table;
    EXPECT_TRUE(OpenTable(&table).IsCorruption()) << offset;
  }
  ASSERT_TRUE(WriteStringToFileSync(&env_, pristine.substr(0, 20), path_).ok());
  std::unique_ptr<TableReader> table;
  EXPECT_TRUE(OpenTable(&table).IsCorruption());
}

TEST_F(TableTest, WorksWithBloomDisabledAndEmptyTable) {
  options_.bloom_bits_per_key = 0;
  Build({{IKey("a", 1), "1"}, {IKey("b", 1), "2"}});
  std::unique_ptr<TableReader> table;
  ASSERT_TRUE(OpenTable(&table).ok());
  EXPECT_FALSE(table->has_filter());
  EXPECT_TRUE(table->KeyMayMatch("zzz"));
  TableReader::GetResult result;
  std::string value;
  ASSERT_TRUE(table->Get(MakeLookupKey("b", 9), &result, &value).ok());
  EXPECT_EQ(value, "2");

  Build({});
  ASSERT_TRUE(OpenTable(&table).ok());
  auto it = table->NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

TEST(BloomTest, NoFalseNegativesAndFalsePositiveRateNearTheory) {
  constexpr int kKeys = 10000;
  constexpr int kProbes = 100000;
  BloomFilterBuilder builder(10);
  for (int i = 0; i < kKeys; i++) builder.AddKey(Key(i));
  std::string filter = builder.Finish();
  for (int i = 0; i < kKeys; i++) ASSERT_TRUE(BloomKeyMayMatch(filter, Key(i)));
  int false_positives = 0;
  for (int i = 0; i < kProbes; i++) false_positives += BloomKeyMayMatch(filter, Key(kKeys + i));
  const double k = static_cast<uint8_t>(filter.back());
  const double bits = (filter.size() - 1) * 8.0;
  const double theory = std::pow(1 - std::exp(-k * kKeys / bits), k);
  const double measured = static_cast<double>(false_positives) / kProbes;
  RecordProperty("measured_fp_rate", std::to_string(measured));
  EXPECT_EQ(k, 7);
  EXPECT_LE(measured, 2 * theory) << "theory " << theory;
  EXPECT_GT(measured, 0.0);
}

TEST(BloomTest, TinyAndMalformedFilters) {
  BloomFilterBuilder builder(10);
  builder.AddKey("only");
  std::string filter = builder.Finish();
  EXPECT_EQ(filter.size(), 9u);  // 64-bit minimum + k byte
  EXPECT_TRUE(BloomKeyMayMatch(filter, "only"));
  EXPECT_TRUE(BloomKeyMayMatch("", "x"));           // malformed: always "maybe"
  EXPECT_TRUE(BloomKeyMayMatch("\xff\xff\x7f", "x"));  // k out of range
}

std::shared_ptr<const Block> MakeBlock(size_t size) {
  return std::make_shared<const Block>(std::string(size, 'b'));
}

TEST(BlockCacheTest, LruEvictionOrderWithinAShard) {
  BlockCache cache(16 * 3000);  // 3000 bytes per shard
  // Find four offsets of file 1 that land in the same shard.
  std::vector<uint64_t> offsets;
  int shard = BlockCache::ShardFor(1, 0);
  for (uint64_t off = 0; offsets.size() < 4; off += 4096) {
    if (BlockCache::ShardFor(1, off) == shard) offsets.push_back(off);
  }
  for (int i = 0; i < 3; i++) cache.Insert(1, offsets[i], MakeBlock(1000));
  ASSERT_NE(cache.Lookup(1, offsets[0]), nullptr);  // offsets[0] becomes most recent
  cache.Insert(1, offsets[3], MakeBlock(1000));     // evicts the LRU entry: offsets[1]
  EXPECT_NE(cache.Lookup(1, offsets[0]), nullptr);
  EXPECT_EQ(cache.Lookup(1, offsets[1]), nullptr);
  EXPECT_NE(cache.Lookup(1, offsets[2]), nullptr);
  EXPECT_NE(cache.Lookup(1, offsets[3]), nullptr);
  EXPECT_EQ(cache.evictions(), 1u);
  EXPECT_EQ(cache.hits(), 4u);
  EXPECT_EQ(cache.misses(), 1u);
}

TEST(BlockCacheTest, CapacityShardingAndHeldBlocksStayValid) {
  BlockCache cache(16 * 10000);
  std::shared_ptr<const Block> held;
  for (uint64_t off = 0; off < 2000 * 4096; off += 4096) {
    auto block = MakeBlock(1000);
    if (off == 0) held = block;
    cache.Insert(3, off, block);
  }
  EXPECT_LE(cache.usage(), 16u * 10000);
  EXPECT_GT(cache.usage(), 16u * 10000 / 2);  // all shards are used
  EXPECT_EQ(cache.Lookup(3, 0), nullptr);     // long evicted...
  EXPECT_EQ(held->data(), std::string(1000, 'b'));  // ...but still valid for its holder
  cache.Insert(3, 1, MakeBlock(20000));  // bigger than a shard: not cached
  EXPECT_EQ(cache.Lookup(3, 1), nullptr);
}

TEST_F(TableTest, ReadsGoThroughTheBlockCache) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 1000; i++) entries.emplace_back(IKey(Key(i), 1), std::string(50, 'v'));
  Build(entries);
  BlockCache cache(1 << 20);
  std::unique_ptr<TableReader> table;
  ASSERT_TRUE(OpenTable(&table, &cache).ok());
  TableReader::GetResult result;
  std::string value;
  ASSERT_TRUE(table->Get(MakeLookupKey(Key(5), 9), &result, &value).ok());
  ASSERT_TRUE(table->Get(MakeLookupKey(Key(6), 9), &result, &value).ok());
  EXPECT_EQ(cache.misses(), 1u);
  EXPECT_EQ(cache.hits(), 1u);
}

}  // namespace
}  // namespace docstore::kv
