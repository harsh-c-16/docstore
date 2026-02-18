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
  for (int i = 0; i < 1000; i++) {
    Put("key" + std::to_string(i % 50), "value" + std::to_string(i));
    ASSERT_TRUE(engine_->Flush().ok());
  }
  auto check = [&] {
    auto stats = engine_->Stats();
    int tables = stats["l0_files"].get<int>() + stats["l1_files"].get<int>();
    EXPECT_EQ(static_cast<int>(Files(FileType::kTable).size()), tables);
    EXPECT_EQ(Files(FileType::kLog).size(), 1u);
    EXPECT_EQ(Files(FileType::kManifest).size(), 1u);
    EXPECT_EQ(Files(FileType::kTemp).size(), 0u);
    for (int k = 0; k < 50; k++) {
      EXPECT_EQ(Get("key" + std::to_string(k)), "value" + std::to_string(950 + k));
    }
  };
  check();
  Open();
  check();
}

TEST_F(EngineTest, ManyFlushesAndReopens) {
  options_.memtable_size = 4 << 10;
  Open();
  for (int round = 0; round < 20; round++) {
    for (int i = 0; i < 300; i++) Put("k" + std::to_string(i), std::to_string(round));
    Open();
    for (int i = 0; i < 300; i++) ASSERT_EQ(Get("k" + std::to_string(i)), std::to_string(round));
  }
  EXPECT_GT(engine_->Stats()["l0_files"].get<int>(), 1);
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
