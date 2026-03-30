// Concurrency stress: one writer repeatedly commits a single batch that sets 1,000 keys to the
// same version v, while 8 readers keep scanning and reading. Every scan must see all 1,000 keys
// at one version (snapshot consistency + atomic batch visibility), and the versions each reader
// sees must never go backwards. Tiny memtables keep flushes and compactions running throughout.
// 3 s in the fast suite, 30 s in the long suite; meant to be run under TSan as well.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "env/mem_env.h"
#include "kv/engine.h"
#include "test_util.h"
#include "util/random.h"

namespace docstore::kv {
namespace {

constexpr int kKeys = 1000;

std::string Key(int i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "key%04d", i);
  return buf;
}

std::string Value(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "v%010llu", static_cast<unsigned long long>(v));
  return buf;
}

uint64_t VersionOf(std::string_view value) { return std::stoull(std::string(value.substr(1))); }

TEST(StressTest, ScansSeeWholeBatchesAndMonotonicVersions) {
  const auto duration = std::chrono::seconds(test::LongMode() ? 30 : 3);
  MemEnv env;
  Options options;
  options.env = &env;
  options.memtable_size = 64 << 10;  // a flush every few batches
  options.target_file_size = 64 << 10;
  options.journal_commit_interval_ms = 5;
  std::unique_ptr<Engine> engine;
  ASSERT_TRUE(Engine::Open(options, "/db", &engine).ok());

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> errors{0}, scans{0}, gets{0}, batches{0};
  std::vector<std::string> first_error(9);

  std::vector<std::thread> readers;
  for (int r = 0; r < 8; r++) {
    readers.emplace_back([&, r] {
      Random rnd(r);
      uint64_t last_seen = 0;
      auto fail = [&](const std::string& what) {
        if (errors.fetch_add(1) == 0) first_error[r] = what;
      };
      while (!stop) {
        if (rnd.OneIn(2)) {
          auto it = engine->NewIterator(ReadOptions());
          int count = 0;
          uint64_t version = 0;
          for (; it->Valid(); it->Next(), count++) {
            uint64_t v = VersionOf(it->value());
            if (count == 0) version = v;
            if (it->key() != Key(count) || v != version) {
              fail("scan mixed versions or keys at " + std::string(it->key()));
              break;
            }
          }
          if (!it->status().ok()) fail(it->status().ToString());
          if (count != 0 && count != kKeys) fail("scan saw " + std::to_string(count) + " keys");
          if (count == kKeys) {
            if (version < last_seen) fail("scan went back in time");
            last_seen = version;
          }
          scans++;
        } else {
          std::string value;
          Status s = engine->Get(Key(static_cast<int>(rnd.Uniform(kKeys))), &value);
          if (s.ok()) {
            if (VersionOf(value) < last_seen) fail("get went back in time");
            last_seen = VersionOf(value);
          } else if (!s.IsNotFound() || last_seen != 0) {
            fail("get: " + s.ToString());
          }
          gets++;
        }
      }
    });
  }

  std::thread writer([&] {
    for (uint64_t v = 1; !stop; v++) {
      WriteBatch batch;
      for (int i = 0; i < kKeys; i++) batch.Put(Key(i), Value(v));
      Status s = engine->Write(WriteOptions(), &batch);
      if (!s.ok() && errors.fetch_add(1) == 0) first_error[8] = s.ToString();
      batches++;
    }
  });

  std::this_thread::sleep_for(duration);
  stop = true;
  writer.join();
  for (auto& t : readers) t.join();
  auto stats = engine->Stats();
  std::string msg;
  for (const auto& e : first_error) msg += e;
  EXPECT_EQ(errors.load(), 0u) << msg;
  EXPECT_GT(stats["flushes"].get<int>(), 0);
  EXPECT_GT(stats["compactions"].get<int>(), 0);
  std::printf(
      "stress: %llu batches x %d keys, %llu scans, %llu gets, %d flushes, %d compactions, "
      "0 inconsistent reads\n",
      static_cast<unsigned long long>(batches.load()), kKeys,
      static_cast<unsigned long long>(scans.load()), static_cast<unsigned long long>(gets.load()),
      stats["flushes"].get<int>(), stats["compactions"].get<int>());
}

}  // namespace
}  // namespace docstore::kv
