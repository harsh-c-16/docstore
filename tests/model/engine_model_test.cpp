// Randomized test of the engine against std::map. Every op is checked; on failure the seed and
// op index are printed so the run can be replayed with DOCSTORE_TEST_SEED=<seed>.

#include <gtest/gtest.h>

#include <map>
#include <memory>

#include "env/mem_env.h"
#include "kv/engine.h"
#include "test_util.h"
#include "util/random.h"

namespace docstore::kv {
namespace {

class EngineModel {
 public:
  EngineModel(uint64_t seed, Options options) : rnd_(seed), options_(std::move(options)) {
    options_.env = &env_;
    Reopen();
  }

  void Reopen() {
    engine_.reset();
    Status s = Engine::Open(options_, "/db", &engine_);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  std::string RandomKey() { return "key" + std::to_string(rnd_.Uniform(500)); }

  // One random operation; returns a description for failure messages.
  void Step(int i) {
    SCOPED_TRACE("op " + std::to_string(i));
    uint64_t r = rnd_.Uniform(100);
    if (r < 45) {
      std::string k = RandomKey();
      std::string v = "v" + std::to_string(i) + std::string(rnd_.Uniform(100), 'x');
      ASSERT_TRUE(engine_->Put(WriteOptions(), k, v).ok());
      model_[k] = v;
    } else if (r < 60) {
      std::string k = RandomKey();
      ASSERT_TRUE(engine_->Delete(WriteOptions(), k).ok());
      model_.erase(k);
    } else if (r < 70) {
      // Multi-key batch: applied atomically.
      WriteBatch batch;
      for (int j = 0; j < 5; j++) {
        std::string k = RandomKey();
        if (rnd_.OneIn(3)) {
          batch.Delete(k);
          model_.erase(k);
        } else {
          batch.Put(k, "batch" + std::to_string(i));
          model_[k] = "batch" + std::to_string(i);
        }
      }
      ASSERT_TRUE(engine_->Write(WriteOptions(), &batch).ok());
    } else if (r < 98) {
      std::string k = RandomKey();
      std::string got;
      Status s = engine_->Get(k, &got);
      auto it = model_.find(k);
      if (it == model_.end()) {
        ASSERT_TRUE(s.IsNotFound()) << k << " " << s.ToString();
      } else {
        ASSERT_TRUE(s.ok()) << k << " " << s.ToString();
        ASSERT_EQ(got, it->second) << k;
      }
    } else {
      Reopen();
    }
  }

  void VerifyAll() {
    for (int k = 0; k < 500; k++) {
      std::string key = "key" + std::to_string(k);
      std::string got;
      Status s = engine_->Get(key, &got);
      auto it = model_.find(key);
      if (it == model_.end()) {
        ASSERT_TRUE(s.IsNotFound()) << key;
      } else {
        ASSERT_TRUE(s.ok()) << key << " " << s.ToString();
        ASSERT_EQ(got, it->second);
      }
    }
  }

 private:
  Random rnd_;
  Options options_;
  MemEnv env_;
  std::unique_ptr<Engine> engine_;
  std::map<std::string, std::string> model_;
};

TEST(EngineModelTest, RandomOpsWithReopen) {
  const int seeds = test::LongMode() ? 100 : 20;
  const int ops = test::LongMode() ? 50000 : 10000;
  const uint64_t base = test::TestSeed();
  for (int s = 0; s < seeds; s++) {
    uint64_t seed = base + s;
    SCOPED_TRACE("seed " + std::to_string(seed) + " (rerun with DOCSTORE_TEST_SEED=" +
                 std::to_string(seed) + ")");
    Options options;
    options.memtable_size = 4 << 10;
    options.target_file_size = 16 << 10;
    EngineModel model(seed, options);
    for (int i = 0; i < ops && !::testing::Test::HasFatalFailure(); i++) model.Step(i);
    if (::testing::Test::HasFatalFailure()) return;
    model.Reopen();
    model.VerifyAll();
    if (::testing::Test::HasFatalFailure()) return;
  }
}

}  // namespace
}  // namespace docstore::kv
