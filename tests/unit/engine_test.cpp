#include <gtest/gtest.h>

#include <memory>

#include "env/mem_env.h"
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

  std::string Get(const std::string& key) {
    std::string v;
    Status s = engine_->Get(key, &v);
    return s.ok() ? v : s.CodeName() == "NotFound" ? "NOT_FOUND" : s.ToString();
  }

  // Highest-numbered WAL in the directory.
  std::string NewestLog() {
    std::vector<std::string> children;
    EXPECT_TRUE(env_.GetChildren("/db", &children).ok());
    uint64_t best = 0;
    for (const auto& c : children) {
      uint64_t n;
      if (ParseFileName(c, &n) == FileType::kLog && n > best) best = n;
    }
    return LogFileName("/db", best);
  }

  MemEnv env_;
  Options options_;
  std::unique_ptr<Engine> engine_;
};

TEST_F(EngineTest, PutGetDeleteOverwrite) {
  WriteOptions wo;
  ASSERT_TRUE(engine_->Put(wo, "a", "1").ok());
  ASSERT_TRUE(engine_->Put(wo, "b", "2").ok());
  ASSERT_TRUE(engine_->Put(wo, "a", "3").ok());
  ASSERT_TRUE(engine_->Delete(wo, "b").ok());
  EXPECT_EQ(Get("a"), "3");
  EXPECT_EQ(Get("b"), "NOT_FOUND");
  EXPECT_EQ(Get("c"), "NOT_FOUND");
  WriteBatch empty;
  EXPECT_TRUE(engine_->Write(wo, &empty).ok());
}

TEST_F(EngineTest, TenThousandWritesSurviveReopen) {
  for (int i = 0; i < 10000; i++) {
    ASSERT_TRUE(engine_->Put(WriteOptions(), "k" + std::to_string(i), "v" + std::to_string(i)).ok());
  }
  Open();
  for (int i = 0; i < 10000; i++) ASSERT_EQ(Get("k" + std::to_string(i)), "v" + std::to_string(i));
  // Sequence numbers continue after recovery: a new write must shadow the recovered one.
  ASSERT_TRUE(engine_->Put(WriteOptions(), "k0", "new").ok());
  Open();
  EXPECT_EQ(Get("k0"), "new");
}

TEST_F(EngineTest, TornTailInNewestWalIsDropped) {
  for (int i = 0; i < 100; i++) {
    ASSERT_TRUE(engine_->Put(WriteOptions(), "k" + std::to_string(i), "value").ok());
  }
  engine_.reset();
  std::string log = NewestLog(), contents;
  ASSERT_TRUE(ReadFileToString(&env_, log, &contents).ok());
  ASSERT_TRUE(WriteStringToFileSync(&env_, contents.substr(0, contents.size() - 3), log).ok());
  Open();
  for (int i = 0; i < 99; i++) EXPECT_EQ(Get("k" + std::to_string(i)), "value") << i;
  EXPECT_EQ(Get("k99"), "NOT_FOUND");  // the torn record is gone, not garbage
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
  engine.reset();
  ASSERT_TRUE(Engine::Open(options, dir, &engine).ok());
  std::string v;
  ASSERT_TRUE(engine->Get("k", &v).ok());
  EXPECT_EQ(v, "v");
}

}  // namespace
}  // namespace docstore::kv
