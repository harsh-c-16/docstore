#include <gtest/gtest.h>

#include <memory>

#include "env/env.h"
#include "env/fault_injection_env.h"
#include "env/mem_env.h"
#include "test_util.h"

namespace docstore {
namespace {

std::string ReadAll(Env* env, const std::string& path) {
  std::string out;
  Status s = ReadFileToString(env, path, &out);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return out;
}

// The same behavioural contract must hold for the real and the in-memory filesystem.
class EnvContractTest : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    if (GetParam()) {
      env_ = Env::Default();
      dir_ = test::NewTempDir("env");
    } else {
      mem_ = std::make_unique<MemEnv>();
      env_ = mem_.get();
      dir_ = "/db";
      ASSERT_TRUE(env_->CreateDir(dir_).ok());
    }
  }

  std::unique_ptr<MemEnv> mem_;
  Env* env_ = nullptr;
  std::string dir_;
};

TEST_P(EnvContractTest, WriteReadRenameDelete) {
  std::string a = dir_ + "/a", b = dir_ + "/b";
  std::unique_ptr<WritableFile> f;
  ASSERT_TRUE(env_->NewWritableFile(a, &f).ok());
  ASSERT_TRUE(f->Append("hello ").ok());
  std::string big(100000, 'x');  // larger than the 64 KiB PosixEnv buffer
  ASSERT_TRUE(f->Append(big).ok());
  ASSERT_TRUE(f->Append("!").ok());
  ASSERT_TRUE(f->Flush().ok());
  ASSERT_TRUE(f->Sync().ok());
  ASSERT_TRUE(f->Close().ok());
  EXPECT_EQ(ReadAll(env_, a), "hello " + big + "!");

  uint64_t size = 0;
  ASSERT_TRUE(env_->GetFileSize(a, &size).ok());
  EXPECT_EQ(size, 6 + big.size() + 1);

  std::unique_ptr<RandomAccessFile> r;
  ASSERT_TRUE(env_->NewRandomAccessFile(a, &r).ok());
  std::string got;
  ASSERT_TRUE(r->Read(2, 4, &got).ok());
  EXPECT_EQ(got, "llo ");
  ASSERT_TRUE(r->Read(size - 1, 10, &got).ok());  // short read at EOF is not an error
  EXPECT_EQ(got, "!");

  std::unique_ptr<SequentialFile> seq;
  ASSERT_TRUE(env_->NewSequentialFile(a, &seq).ok());
  ASSERT_TRUE(seq->Skip(6).ok());
  ASSERT_TRUE(seq->Read(3, &got).ok());
  EXPECT_EQ(got, "xxx");

  ASSERT_TRUE(env_->RenameFile(a, b).ok());
  EXPECT_FALSE(env_->FileExists(a));
  EXPECT_TRUE(env_->FileExists(b));
  std::vector<std::string> children;
  ASSERT_TRUE(env_->GetChildren(dir_, &children).ok());
  EXPECT_EQ(children, std::vector<std::string>{"b"});
  ASSERT_TRUE(env_->SyncDir(dir_).ok());
  ASSERT_TRUE(env_->DeleteFile(b).ok());
  EXPECT_FALSE(env_->FileExists(b));
  EXPECT_TRUE(env_->DeleteFile(b).IsNotFound());
  std::unique_ptr<SequentialFile> missing;
  EXPECT_TRUE(env_->NewSequentialFile(b, &missing).IsNotFound());
}

TEST_P(EnvContractTest, LockPreventsDoubleOpen) {
  std::string lock_path = dir_ + "/LOCK";
  std::unique_ptr<FileLock> first, second;
  ASSERT_TRUE(env_->LockFile(lock_path, &first).ok());
  EXPECT_TRUE(env_->LockFile(lock_path, &second).IsIOError());
  ASSERT_TRUE(env_->UnlockFile(std::move(first)).ok());
  EXPECT_TRUE(env_->LockFile(lock_path, &second).ok());
}

INSTANTIATE_TEST_SUITE_P(PosixAndMem, EnvContractTest, ::testing::Values(true, false),
                         [](const auto& info) { return info.param ? "Posix" : "Mem"; });

class FaultInjectionEnvTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(env_.CreateDir("/db").ok()); }

  void Write(const std::string& path, std::string_view data, bool sync) {
    std::unique_ptr<WritableFile> f;
    ASSERT_TRUE(env_.NewWritableFile(path, &f).ok());
    ASSERT_TRUE(f->Append(data).ok());
    if (sync) {
      ASSERT_TRUE(f->Sync().ok());
    }
  }

  void Crash() {
    env_.SimulatePowerLoss();
    env_.PowerOn();
  }

  FaultInjectionEnv env_;
};

TEST_F(FaultInjectionEnvTest, UnsyncedDataVanishes) {
  std::unique_ptr<WritableFile> f;
  ASSERT_TRUE(env_.NewWritableFile("/db/log", &f).ok());
  ASSERT_TRUE(env_.SyncDir("/db").ok());
  ASSERT_TRUE(f->Append("durable").ok());
  ASSERT_TRUE(f->Sync().ok());
  ASSERT_TRUE(f->Append("-volatile").ok());
  EXPECT_EQ(ReadAll(&env_, "/db/log"), "durable-volatile");
  Crash();
  EXPECT_EQ(ReadAll(&env_, "/db/log"), "durable");
}

TEST_F(FaultInjectionEnvTest, NewFileNeedsDirSyncEvenIfDataSynced) {
  Write("/db/a", "synced data", /*sync=*/true);
  Crash();
  EXPECT_FALSE(env_.FileExists("/db/a"));

  Write("/db/b", "synced data", /*sync=*/true);
  ASSERT_TRUE(env_.SyncDir("/db").ok());
  Crash();
  EXPECT_EQ(ReadAll(&env_, "/db/b"), "synced data");
}

TEST_F(FaultInjectionEnvTest, UnsyncedRenameAndDeleteRevert) {
  Write("/db/CURRENT", "old", true);
  Write("/db/CURRENT.tmp", "new", true);
  ASSERT_TRUE(env_.SyncDir("/db").ok());
  ASSERT_TRUE(env_.RenameFile("/db/CURRENT.tmp", "/db/CURRENT").ok());
  EXPECT_EQ(ReadAll(&env_, "/db/CURRENT"), "new");
  Crash();
  EXPECT_EQ(ReadAll(&env_, "/db/CURRENT"), "old");  // rename was not durable
  EXPECT_TRUE(env_.FileExists("/db/CURRENT.tmp"));

  ASSERT_TRUE(env_.RenameFile("/db/CURRENT.tmp", "/db/CURRENT").ok());
  ASSERT_TRUE(env_.SyncDir("/db").ok());
  Crash();
  EXPECT_EQ(ReadAll(&env_, "/db/CURRENT"), "new");
  EXPECT_FALSE(env_.FileExists("/db/CURRENT.tmp"));

  ASSERT_TRUE(env_.DeleteFile("/db/CURRENT").ok());
  Crash();
  EXPECT_TRUE(env_.FileExists("/db/CURRENT"));  // delete was not durable either
}

TEST_F(FaultInjectionEnvTest, PoweredOffRejectsMutations) {
  std::unique_ptr<WritableFile> f;
  ASSERT_TRUE(env_.NewWritableFile("/db/x", &f).ok());
  env_.SimulatePowerLoss();
  EXPECT_TRUE(f->Append("late").IsIOError());
  EXPECT_TRUE(f->Sync().IsIOError());
  std::unique_ptr<WritableFile> g;
  EXPECT_TRUE(env_.NewWritableFile("/db/y", &g).IsIOError());
  env_.PowerOn();
  EXPECT_TRUE(env_.NewWritableFile("/db/y", &g).ok());
}

TEST_F(FaultInjectionEnvTest, InjectedWriteAndSyncFailures) {
  std::unique_ptr<WritableFile> f;
  ASSERT_TRUE(env_.NewWritableFile("/db/x", &f).ok());
  env_.FailWritesAfter(2);
  EXPECT_TRUE(f->Append("1").ok());
  EXPECT_TRUE(f->Append("2").ok());
  EXPECT_TRUE(f->Append("3").IsIOError());
  env_.FailSyncs(true);
  EXPECT_TRUE(f->Sync().IsIOError());
  EXPECT_TRUE(env_.SyncDir("/db").IsIOError());
  env_.ClearFaults();
  EXPECT_TRUE(f->Append("3").ok());
  EXPECT_TRUE(f->Sync().ok());
  EXPECT_EQ(ReadAll(&env_, "/db/x"), "123");
}

}  // namespace
}  // namespace docstore
