// I/O error tests: make writes or syncs fail during flush, compaction and the WAL path. The
// engine must turn the failure into a background error (writes -> ReadOnly, reads still work),
// and after a reopen every acknowledged write must be intact.

#include <gtest/gtest.h>

#include <map>
#include <memory>

#include "env/fault_injection_env.h"
#include "kv/engine.h"

namespace docstore::kv {
namespace {

class IoErrorTest : public ::testing::Test {
 protected:
  void SetUp() override { options_.env = &env_; }

  void Open() {
    engine_.reset();
    Status s = Engine::Open(options_, "/db", &engine_);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  void Write(int from, int to) {
    for (int i = from; i < to; i++) {
      std::string k = "k" + std::to_string(i % 700), v = "v" + std::to_string(i);
      ASSERT_TRUE(engine_->Put(WriteOptions(), k, v).ok());
      model_[k] = v;
    }
  }

  void Verify() {
    for (const auto& [k, v] : model_) {
      std::string got;
      Status s = engine_->Get(k, &got);
      ASSERT_TRUE(s.ok()) << k << " " << s.ToString();
      ASSERT_EQ(got, v);
    }
  }

  // Runs `background_op` with the n-th append failing; checks ReadOnly, reads, then recovery.
  void FailDuring(const std::function<Status()>& background_op, int n) {
    SCOPED_TRACE("fail after " + std::to_string(n) + " appends");
    env_.FailWritesAfter(n);
    Status s = background_op();
    env_.FailWritesAfter(-1);
    if (s.ok()) {
      successes_++;  // n was past the last append of this operation
      return;
    }
    EXPECT_TRUE(engine_->Put(WriteOptions(), "x", "y").IsReadOnly());
    EXPECT_NE(engine_->Stats()["bg_error"], "");
    Verify();  // reads keep working from memory and the old tables
    env_.ClearFaults();
    Open();
    Verify();
    Write(5000, 5100);  // and the reopened database accepts writes again
    failures_++;
  }

  FaultInjectionEnv env_;
  Options options_;
  std::unique_ptr<Engine> engine_;
  std::map<std::string, std::string> model_;
  int failures_ = 0;
  int successes_ = 0;
};

TEST_F(IoErrorTest, FailedFlushWritesBecomeReadOnlyAndRecover) {
  Open();
  // Every injection point from the first append of the flush until past its last one.
  for (int n = 0; successes_ < 2; n++) {
    ASSERT_LT(n, 200);
    Write(n * 100, n * 100 + 700);
    FailDuring([&] { return engine_->Flush(); }, n);
  }
  EXPECT_GE(failures_, 3);
}

TEST_F(IoErrorTest, FailedCompactionWritesBecomeReadOnlyAndRecover) {
  options_.l0_compaction_trigger = 100;  // only the explicit CompactAll compacts
  Open();
  for (int n = 0; successes_ < 2; n += 2) {
    ASSERT_LT(n, 400);
    Write(n * 100, n * 100 + 700);
    ASSERT_TRUE(engine_->Flush().ok());
    Write(n * 100 + 700, n * 100 + 1400);
    ASSERT_TRUE(engine_->Flush().ok());
    FailDuring([&] { return engine_->CompactAll(); }, n);
  }
  EXPECT_GE(failures_, 3);
}

TEST_F(IoErrorTest, FailedWalSyncFailsTheWriteAndStopsWrites) {
  Open();
  Write(0, 100);
  env_.FailSyncs(true);
  Status s = engine_->Put(WriteOptions{.sync = true}, "synced", "1");
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_TRUE(engine_->Put(WriteOptions(), "after", "1").IsReadOnly());
  env_.ClearFaults();
  Open();
  Verify();  // "synced" itself may or may not have survived: its write returned an error
}

TEST_F(IoErrorTest, FailedWalAppendFailsTheWrite) {
  Open();
  Write(0, 100);
  env_.FailWritesAfter(0);
  EXPECT_TRUE(engine_->Put(WriteOptions(), "lost", "1").IsIOError());
  env_.ClearFaults();
  EXPECT_TRUE(engine_->Put(WriteOptions(), "next", "1").IsReadOnly());
  Open();
  Verify();
}

}  // namespace
}  // namespace docstore::kv
