#include <gtest/gtest.h>

#include <memory>

#include "env/fault_injection_env.h"
#include "kv/engine.h"
#include "kv/filename.h"
#include "test_util.h"

namespace docstore::kv {
namespace {

class EngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    options_.env = &env_;
    Open();
  }

  void Open() {
    engine_.reset();
    Status s = Engine::Open(options_, "/db", &engine_);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  Status TryOpen() {
    engine_.reset();
    return Engine::Open(options_, "/db", &engine_);
  }

  std::string Get(const std::string& key) {
    std::string v;
    Status s = engine_->Get(key, &v);
    return s.ok() ? v : s.IsNotFound() ? "NOT_FOUND" : s.ToString();
  }

  void Put(const std::string& k, const std::string& v) {
    ASSERT_TRUE(engine_->Put(WriteOptions(), k, v).ok());
  }

  std::vector<uint64_t> Files(FileType want) {
    std::vector<std::string> children;
    EXPECT_TRUE(env_.GetChildren("/db", &children).ok());
    std::vector<uint64_t> out;
    for (const auto& c : children) {
      uint64_t n;
      if (ParseFileName(c, &n) == want) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  void CorruptFile(const std::string& path, size_t offset) {
    std::string contents;
    ASSERT_TRUE(ReadFileToString(&env_, path, &contents).ok());
    ASSERT_LT(offset, contents.size());
    contents[offset] ^= 0x20;
    ASSERT_TRUE(WriteStringToFileSync(&env_, contents, path).ok());
  }

  FaultInjectionEnv env_;
  Options options_;
  std::unique_ptr<Engine> engine_;
};

TEST_F(EngineTest, PutGetDeleteOverwrite) {
  Put("a", "1");
  Put("b", "2");
  Put("a", "3");
  ASSERT_TRUE(engine_->Delete(WriteOptions(), "b").ok());
  EXPECT_EQ(Get("a"), "3");
  EXPECT_EQ(Get("b"), "NOT_FOUND");
  EXPECT_EQ(Get("c"), "NOT_FOUND");
  WriteBatch empty;
  EXPECT_TRUE(engine_->Write(WriteOptions(), &empty).ok());
}

TEST_F(EngineTest, TenThousandWritesSurviveReopen) {
  for (int i = 0; i < 10000; i++) Put("k" + std::to_string(i), "v" + std::to_string(i));
  Open();
  for (int i = 0; i < 10000; i++) ASSERT_EQ(Get("k" + std::to_string(i)), "v" + std::to_string(i));
  // Sequence numbers continue after recovery: a new write must shadow the recovered one.
  Put("k0", "new");
  Open();
  EXPECT_EQ(Get("k0"), "new");
}

TEST_F(EngineTest, ReadsSpanMemtableAndTables) {
  Put("a", "old");
  Put("b", "keep");
  ASSERT_TRUE(engine_->Flush().ok());
  Put("a", "new");
  ASSERT_TRUE(engine_->Delete(WriteOptions(), "b").ok());
  EXPECT_EQ(Get("a"), "new");      // memtable shadows the table
  EXPECT_EQ(Get("b"), "NOT_FOUND");  // memtable tombstone shadows the table
  ASSERT_TRUE(engine_->Flush().ok());
  EXPECT_EQ(Get("a"), "new");      // newer L0 file wins
  EXPECT_EQ(Get("b"), "NOT_FOUND");  // tombstone kept in L0
  EXPECT_EQ(engine_->Stats()["l0_files"], 2);
  Open();
  EXPECT_EQ(Get("a"), "new");
  EXPECT_EQ(Get("b"), "NOT_FOUND");
}

TEST_F(EngineTest, NoFileLeaksAfterAThousandFlushes) {
  // Automatic compaction off so no background work races the file counts below; the second
  // phase then checks the compaction path explicitly.
  options_.l0_compaction_trigger = 100000;
  options_.l0_slowdown_trigger = 100000;
  options_.l0_stop_trigger = 100000;
  Open();
  for (int i = 0; i < 1000; i++) {
    Put("key" + std::to_string(i % 50), "value" + std::to_string(i));
    ASSERT_TRUE(engine_->Flush().ok());
  }
  auto check = [&](int expected_tables) {
    auto stats = engine_->Stats();
    int tables = stats["l0_files"].get<int>() + stats["l1_files"].get<int>();
    EXPECT_EQ(tables, expected_tables);
    EXPECT_EQ(static_cast<int>(Files(FileType::kTable).size()), tables);
    EXPECT_EQ(Files(FileType::kLog).size(), 1u);
    EXPECT_EQ(Files(FileType::kManifest).size(), 1u);
    EXPECT_EQ(Files(FileType::kTemp).size(), 0u);
    for (int k = 0; k < 50; k++) {
      EXPECT_EQ(Get("key" + std::to_string(k)), "value" + std::to_string(950 + k));
    }
  };
  check(1000);
  ASSERT_TRUE(engine_->CompactAll().ok());
  check(1);  // 1,000 inputs deleted once the compaction was installed
  Open();
  check(1);
}

TEST_F(EngineTest, ManyFlushesAndReopens) {
  options_.memtable_size = 4 << 10;
  Open();
  for (int round = 0; round < 20; round++) {
    for (int i = 0; i < 300; i++) Put("k" + std::to_string(i), std::to_string(round));
    Open();
    for (int i = 0; i < 300; i++) ASSERT_EQ(Get("k" + std::to_string(i)), std::to_string(round));
  }
  auto stats = engine_->Stats();
  EXPECT_GE(stats["l0_files"].get<int>() + stats["l1_files"].get<int>(), 1);
}

TEST_F(EngineTest, TornTailInNewestWalIsDropped) {
  for (int i = 0; i < 100; i++) Put("k" + std::to_string(i), "value");
  engine_.reset();
  std::string log = LogFileName("/db", Files(FileType::kLog).back()), contents;
  ASSERT_TRUE(ReadFileToString(&env_, log, &contents).ok());
  ASSERT_TRUE(WriteStringToFileSync(&env_, contents.substr(0, contents.size() - 3), log).ok());
  Open();
  for (int i = 0; i < 99; i++) EXPECT_EQ(Get("k" + std::to_string(i)), "value") << i;
  EXPECT_EQ(Get("k99"), "NOT_FOUND");  // the torn record is gone, not garbage
}

// Makes the next flush fail before its table is written, so the immutable memtable's WAL
// stays behind as an *older* WAL next to the new one.
void FailNextFlush(Options* options, FaultInjectionEnv* env) {
  options->test_hook = [env](std::string_view point) {
    if (point == "flush:begin") env->FailWritesAfter(0);
  };
}

TEST_F(EngineTest, FailedFlushKeepsDataAndRefusesWrites) {
  FailNextFlush(&options_, &env_);
  Open();
  for (int i = 0; i < 100; i++) Put("k" + std::to_string(i), "v");
  EXPECT_FALSE(engine_->Flush().ok());
  EXPECT_TRUE(engine_->Put(WriteOptions(), "x", "y").IsReadOnly());
  EXPECT_EQ(Get("k5"), "v");  // reads keep working
  EXPECT_NE(engine_->Stats()["bg_error"], "");
  options_.test_hook = nullptr;
  env_.ClearFaults();
  Open();
  for (int i = 0; i < 100; i++) EXPECT_EQ(Get("k" + std::to_string(i)), "v");
  EXPECT_TRUE(engine_->Put(WriteOptions(), "x", "y").ok());
}

TEST_F(EngineTest, CorruptionInOlderWalIsReported) {
  FailNextFlush(&options_, &env_);
  Open();
  for (int i = 0; i < 100; i++) Put("k" + std::to_string(i), std::string(50, 'v'));
  (void)engine_->Flush();
  engine_.reset();
  env_.ClearFaults();
  options_.test_hook = nullptr;
  auto logs = Files(FileType::kLog);
  ASSERT_EQ(logs.size(), 2u);
  CorruptFile(LogFileName("/db", logs[0]), 2000);
  EXPECT_TRUE(TryOpen().IsCorruption());

  options_.paranoid_checks = false;  // skip the damaged rest of the old WAL instead
  Open();
  EXPECT_EQ(Get("k0"), std::string(50, 'v'));
  EXPECT_EQ(Get("k99"), "NOT_FOUND");
}

TEST_F(EngineTest, CorruptCurrentOrMissingManifestIsCorruption) {
  Put("a", "1");
  engine_.reset();
  std::string current;
  ASSERT_TRUE(ReadFileToString(&env_, "/db/CURRENT", &current).ok());
  ASSERT_TRUE(WriteStringToFileSync(&env_, "MANIFEST-999999\n", "/db/CURRENT").ok());
  EXPECT_TRUE(TryOpen().IsCorruption());
  ASSERT_TRUE(WriteStringToFileSync(&env_, "garbage", "/db/CURRENT").ok());
  EXPECT_TRUE(TryOpen().IsCorruption());
  ASSERT_TRUE(WriteStringToFileSync(&env_, current, "/db/CURRENT").ok());
  Open();
  EXPECT_EQ(Get("a"), "1");
}

TEST_F(EngineTest, CreateIfMissingAndErrorIfExists) {
  engine_.reset();
  Options strict = options_;
  strict.create_if_missing = false;
  std::unique_ptr<Engine> e;
  EXPECT_TRUE(Engine::Open(strict, "/other", &e).IsInvalidArgument());
  Options exclusive = options_;
  exclusive.error_if_exists = true;
  EXPECT_TRUE(Engine::Open(exclusive, "/db", &e).IsInvalidArgument());
}

TEST_F(EngineTest, SecondOpenOfSameDatabaseFails) {
  std::unique_ptr<Engine> second;
  EXPECT_TRUE(Engine::Open(options_, "/db", &second).IsIOError());
}

TEST(EngineEmptyTest, OpensOnRealFilesystem) {
  std::string dir = test::NewTempDir("engine");
  Options options;
  std::unique_ptr<Engine> engine;
  ASSERT_TRUE(Engine::Open(options, dir, &engine).ok());
  ASSERT_TRUE(engine->Put(WriteOptions{.sync = true}, "k", "v").ok());
  ASSERT_TRUE(engine->Flush().ok());
  ASSERT_TRUE(engine->Put(WriteOptions(), "k2", "v2").ok());
  engine.reset();
  ASSERT_TRUE(Engine::Open(options, dir, &engine).ok());
  std::string v;
  ASSERT_TRUE(engine->Get("k", &v).ok());
  EXPECT_EQ(v, "v");
  ASSERT_TRUE(engine->Get("k2", &v).ok());
  EXPECT_EQ(v, "v2");
}

}  // namespace
}  // namespace docstore::kv

namespace docstore::kv {
namespace {

using KVs = std::vector<std::pair<std::string, std::string>>;

TEST_F(EngineTest, ScanHonoursBoundsTombstonesAndLimit) {
  Put("a", "1");
  Put("b", "2");
  Put("c", "3");
  ASSERT_TRUE(engine_->Flush().ok());
  Put("b", "2b");                                       // newer version in the memtable
  ASSERT_TRUE(engine_->Delete(WriteOptions(), "c").ok());  // tombstone over a flushed key
  Put("d", "4");
  KVs out;
  ASSERT_TRUE(engine_->Scan(ReadOptions(), 0, &out).ok());
  EXPECT_EQ(out, (KVs{{"a", "1"}, {"b", "2b"}, {"d", "4"}}));
  ASSERT_TRUE(engine_->Scan(ReadOptions{"b", "d"}, 0, &out).ok());
  EXPECT_EQ(out, (KVs{{"b", "2b"}}));
  ASSERT_TRUE(engine_->Scan(ReadOptions{"a", ""}, 2, &out).ok());
  EXPECT_EQ(out, (KVs{{"a", "1"}, {"b", "2b"}}));
  ASSERT_TRUE(engine_->Scan(ReadOptions{"e", ""}, 0, &out).ok());
  EXPECT_TRUE(out.empty());
}

TEST_F(EngineTest, ScanStaysConsistentWhileWritesAndFlushesHappen) {
  options_.memtable_size = 8 << 10;
  Open();
  for (int i = 0; i < 1000; i++) Put("k" + std::to_string(1000 + i), "v1");
  auto it = engine_->NewIterator(ReadOptions());
  // Rewrite, delete and flush everything after the scan started.
  for (int i = 0; i < 1000; i++) {
    if (i % 3 == 0) {
      ASSERT_TRUE(engine_->Delete(WriteOptions(), "k" + std::to_string(1000 + i)).ok());
    } else {
      Put("k" + std::to_string(1000 + i), "v2");
    }
  }
  Put("k0500", "new key");
  ASSERT_TRUE(engine_->Flush().ok());
  int n = 0;
  for (; it->Valid(); it->Next(), n++) {
    ASSERT_EQ(it->key(), "k" + std::to_string(1000 + n));
    ASSERT_EQ(it->value(), "v1");
  }
  EXPECT_TRUE(it->status().ok());
  EXPECT_EQ(n, 1000);

  // A snapshot's Get and iterator agree with each other, not with later writes.
  auto snap = engine_->GetSnapshot();
  Put("k1001", "v3");
  std::string v;
  ASSERT_TRUE(snap->Get("k1001", &v).ok());
  EXPECT_EQ(v, "v2");
  EXPECT_TRUE(snap->Get("k1000", &v).IsNotFound());
  EXPECT_EQ(Get("k1001"), "v3");
}

TEST_F(EngineTest, ScanReportsCorruptionInsteadOfSkippingData) {
  for (int i = 0; i < 2000; i++) Put("k" + std::to_string(10000 + i), std::string(100, 'v'));
  ASSERT_TRUE(engine_->Flush().ok());
  engine_.reset();
  auto tables = Files(FileType::kTable);
  ASSERT_EQ(tables.size(), 1u);
  CorruptFile(TableFileName("/db", tables[0]), 100);
  Open();  // footer, index and filter are fine; the first data block is not
  KVs out;
  EXPECT_TRUE(engine_->Scan(ReadOptions(), 0, &out).IsCorruption());
  std::string v;
  EXPECT_TRUE(engine_->Get("k10000", &v).IsCorruption());
  EXPECT_TRUE(engine_->Get("k11999", &v).ok());  // undamaged block
}

}  // namespace
}  // namespace docstore::kv

namespace docstore::kv {
namespace {

uint64_t TableBytes(const nlohmann::json& stats) {
  return stats["l0_bytes"].get<uint64_t>() + stats["l1_bytes"].get<uint64_t>();
}

TEST_F(EngineTest, CompactAllMergesIntoL1AndDropsTombstones) {
  for (int i = 0; i < 2000; i++) Put("k" + std::to_string(10000 + i), std::string(100, 'a'));
  ASSERT_TRUE(engine_->Flush().ok());
  for (int i = 0; i < 2000; i += 2) {
    ASSERT_TRUE(engine_->Delete(WriteOptions(), "k" + std::to_string(10000 + i)).ok());
  }
  ASSERT_TRUE(engine_->CompactAll().ok());
  auto stats = engine_->Stats();
  EXPECT_EQ(stats["l0_files"], 0);
  EXPECT_GE(stats["l1_files"].get<int>(), 1);
  EXPECT_GE(stats["compactions"].get<int>(), 1);
  EXPECT_GT(stats["write_amplification"].get<double>(), 0.0);
  // Half the keys and every tombstone are gone, so L1 holds about half the original data.
  EXPECT_LT(TableBytes(stats), 2000u * 100 * 6 / 10);
  KVs out;
  ASSERT_TRUE(engine_->Scan(ReadOptions(), 0, &out).ok());
  ASSERT_EQ(out.size(), 1000u);
  EXPECT_EQ(out[0].first, "k10001");
  Open();
  EXPECT_EQ(Get("k10000"), "NOT_FOUND");
  EXPECT_EQ(Get("k10001"), std::string(100, 'a'));
}

TEST_F(EngineTest, OverwriteWorkloadShrinksOnDiskAfterCompaction) {
  options_.memtable_size = 64 << 10;
  options_.l0_compaction_trigger = 100;  // keep the automatic compactor out of the way
  options_.l0_slowdown_trigger = 200;
  options_.l0_stop_trigger = 300;
  Open();
  for (int round = 0; round < 10; round++) {
    for (int i = 0; i < 1000; i++) Put("key" + std::to_string(i), std::string(200, 'a' + round));
  }
  ASSERT_TRUE(engine_->Flush().ok());
  const uint64_t before = TableBytes(engine_->Stats());
  ASSERT_TRUE(engine_->CompactAll().ok());
  const uint64_t after = TableBytes(engine_->Stats());
  EXPECT_LT(after * 5, before) << before << " -> " << after;  // ~10 versions -> 1
  EXPECT_EQ(Get("key7"), std::string(200, 'j'));
}

TEST_F(EngineTest, AutomaticCompactionKeepsL0SmallAndCutsL1Files) {
  options_.memtable_size = 16 << 10;
  options_.target_file_size = 32 << 10;
  Open();
  for (int i = 0; i < 20000; i++) Put("k" + std::to_string(i % 3000), std::string(64, 'x'));
  ASSERT_TRUE(engine_->Flush().ok());
  auto stats = engine_->Stats();
  EXPECT_GT(stats["compactions"].get<int>(), 0);
  EXPECT_LT(stats["l0_files"].get<int>(), options_.l0_stop_trigger);
  EXPECT_GT(stats["l1_files"].get<int>(), 3);  // ~3000 x 80 bytes cut at 32 KiB
  KVs out;
  ASSERT_TRUE(engine_->Scan(ReadOptions(), 0, &out).ok());
  EXPECT_EQ(out.size(), 3000u);
}

class DropPrefixFilter : public CompactionFilter {
 public:
  bool ShouldDrop(std::string_view key) const override { return key.substr(0, 5) == "dead/"; }
};

TEST_F(EngineTest, CompactionFilterDropsKeys) {
  options_.compaction_filter = std::make_shared<DropPrefixFilter>();
  Open();
  for (int i = 0; i < 100; i++) {
    Put("dead/" + std::to_string(i), "x");
    Put("live/" + std::to_string(i), "y");
  }
  EXPECT_EQ(Get("dead/5"), "x");  // the filter only acts during compaction
  ASSERT_TRUE(engine_->CompactAll().ok());
  EXPECT_EQ(Get("dead/5"), "NOT_FOUND");
  EXPECT_EQ(Get("live/5"), "y");
  KVs out;
  ASSERT_TRUE(engine_->Scan(ReadOptions(), 0, &out).ok());
  EXPECT_EQ(out.size(), 100u);
}

}  // namespace
}  // namespace docstore::kv

namespace docstore::kv {
namespace {

// Regression: with the compaction trigger above the stop trigger, writers used to wait at the
// stop trigger for a compaction that could never start.
TEST_F(EngineTest, StopTriggerBelowCompactionTriggerDoesNotDeadlock) {
  options_.memtable_size = 4 << 10;
  options_.l0_compaction_trigger = 30;
  options_.l0_slowdown_trigger = 2;
  options_.l0_stop_trigger = 3;
  Open();
  for (int i = 0; i < 5000; i++) Put("k" + std::to_string(i % 500), std::string(50, 'v'));
  EXPECT_GT(engine_->Stats()["compactions"].get<int>(), 0);
}

}  // namespace
}  // namespace docstore::kv
