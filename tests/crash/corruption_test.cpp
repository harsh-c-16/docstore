// Corruption tests: flip one random byte in a WAL, SSTable, MANIFEST or CURRENT file. Opening
// or reading must then return Corruption (or, for the newest WAL, drop the torn tail). The
// database must never crash and never return a value that was not written.

#include <gtest/gtest.h>

#include <map>
#include <memory>

#include "env/mem_env.h"
#include "kv/engine.h"
#include "kv/filename.h"
#include "test_util.h"
#include "util/random.h"

namespace docstore::kv {
namespace {

using State = std::map<std::string, std::string>;

struct OpRecord {
  std::string key, value;  // empty value = delete
};

// Builds a database whose directory holds L1 tables, an L0 table, a MANIFEST with several
// edits, CURRENT, and a WAL with unflushed writes. Returns the ops in order and how many of them
// are only in the WAL.
std::vector<OpRecord> BuildDb(Env* env, size_t* wal_ops) {
  Options options;
  options.env = env;
  options.l0_compaction_trigger = 100;  // keep the layout deterministic
  std::unique_ptr<Engine> engine;
  EXPECT_TRUE(Engine::Open(options, "/db", &engine).ok());
  std::vector<OpRecord> ops;
  Random rnd(42);
  auto write = [&](int n, bool deletes) {
    for (int i = 0; i < n; i++) {
      OpRecord op{"key" + std::to_string(rnd.Uniform(2000)), ""};
      if (!deletes || !rnd.OneIn(5))
        op.value = "val" + std::to_string(ops.size()) + std::string(80, 'z');
      Status s = op.value.empty() ? engine->Delete(WriteOptions(), op.key)
                                  : engine->Put(WriteOptions(), op.key, op.value);
      EXPECT_TRUE(s.ok());
      ops.push_back(op);
    }
  };
  write(3000, false);
  EXPECT_TRUE(engine->CompactAll().ok());  // -> L1
  write(1000, true);
  EXPECT_TRUE(engine->Flush().ok());  // -> L0
  write(500, true);                   // -> WAL only
  *wal_ops = 500;
  return ops;
}

State StateAfter(const std::vector<OpRecord>& ops, size_t n) {
  State s;
  for (size_t i = 0; i < n; i++) {
    if (ops[i].value.empty()) {
      s.erase(ops[i].key);
    } else {
      s[ops[i].key] = ops[i].value;
    }
  }
  return s;
}

std::vector<std::string> FilesOfType(Env* env, FileType want) {
  std::vector<std::string> children, out;
  EXPECT_TRUE(env->GetChildren("/db", &children).ok());
  for (const auto& c : children) {
    uint64_t n;
    if (ParseFileName(c, &n) == want) out.push_back("/db/" + c);
  }
  return out;
}

struct Outcome {
  int open_corruption = 0, read_corruption = 0, tail_dropped = 0, undetected_but_correct = 0;
};

void RunTrial(FileType type, Random* rnd, Outcome* outcome) {
  MemEnv env;
  size_t wal_ops = 0;
  std::vector<OpRecord> ops = BuildDb(&env, &wal_ops);
  auto files = FilesOfType(&env, type);
  ASSERT_FALSE(files.empty());
  const std::string victim = files[rnd->Uniform(files.size())];
  std::string contents;
  ASSERT_TRUE(ReadFileToString(&env, victim, &contents).ok());
  const size_t offset = rnd->Uniform(contents.size());
  contents[offset] ^= static_cast<char>(1 + rnd->Uniform(255));
  ASSERT_TRUE(WriteStringToFileSync(&env, contents, victim).ok());
  SCOPED_TRACE(victim + " byte " + std::to_string(offset));

  Options options;
  options.env = &env;
  options.l0_compaction_trigger = 100;
  std::unique_ptr<Engine> engine;
  Status s = Engine::Open(options, "/db", &engine);
  if (!s.ok()) {
    ASSERT_TRUE(s.IsCorruption()) << s.ToString();
    outcome->open_corruption++;
    return;
  }

  // Which states are acceptable: the full state, or (for the WAL) any shorter WAL prefix.
  std::vector<State> allowed;
  size_t min_ops = type == FileType::kLog ? ops.size() - wal_ops : ops.size();
  for (size_t n = min_ops; n <= ops.size(); n++) allowed.push_back(StateAfter(ops, n));

  std::vector<std::pair<std::string, std::string>> rows;
  s = engine->Scan(ReadOptions(), 0, &rows);
  if (!s.ok()) {
    ASSERT_TRUE(s.IsCorruption()) << s.ToString();
    outcome->read_corruption++;
    // Rows returned before the error must be exactly the first rows of the true state.
    const State& full = allowed.back();
    auto it = full.begin();
    for (const auto& row : rows) {
      ASSERT_TRUE(it != full.end());
      ASSERT_EQ(row.first, it->first);
      ASSERT_EQ(row.second, it->second);
      ++it;
    }
  } else {
    State got(rows.begin(), rows.end());
    bool matches = std::find(allowed.begin(), allowed.end(), got) != allowed.end();
    ASSERT_TRUE(matches) << "scan returned a state that was never written";
    if (got == allowed.back()) {
      outcome->undetected_but_correct++;
    } else {
      outcome->tail_dropped++;
    }
  }
  // Point reads: correct value, correct absence, or Corruption. Never anything else.
  const State& full = allowed.back();
  for (int k = 0; k < 2000; k++) {
    std::string key = "key" + std::to_string(k), value;
    s = engine->Get(key, &value);
    if (s.ok()) {
      bool written = std::any_of(allowed.begin(), allowed.end(), [&](const State& st) {
        auto it = st.find(key);
        return it != st.end() && it->second == value;
      });
      ASSERT_TRUE(written) << key << " returned a wrong value";
    } else if (s.IsNotFound()) {
      if (type != FileType::kLog) {
        ASSERT_EQ(full.count(key), 0u) << key << " went missing";
      }
    } else {
      ASSERT_TRUE(s.IsCorruption()) << s.ToString();
    }
  }
}

TEST(CorruptionTest, RandomByteFlipsAreDetectedNeverServed) {
  const int per_type = test::LongMode() ? 250 : 25;
  Random rnd(test::TestSeed());
  const std::pair<FileType, const char*> types[] = {{FileType::kLog, "WAL"},
                                                    {FileType::kTable, "SST"},
                                                    {FileType::kManifest, "MANIFEST"},
                                                    {FileType::kCurrent, "CURRENT"}};
  for (const auto& [type, name] : types) {
    Outcome o;
    for (int i = 0; i < per_type && !::testing::Test::HasFatalFailure(); i++) {
      SCOPED_TRACE(std::string(name) + " trial " + std::to_string(i));
      RunTrial(type, &rnd, &o);
    }
    std::printf(
        "corruption %-8s: %d flips -> open Corruption %d, read Corruption %d, "
        "WAL tail dropped %d, undetected-but-correct %d\n",
        name, per_type, o.open_corruption, o.read_corruption, o.tail_dropped,
        o.undetected_but_correct);
    if (type == FileType::kManifest || type == FileType::kCurrent) {
      EXPECT_EQ(o.open_corruption, per_type) << name << " damage must always fail Open";
    }
  }
}

}  // namespace
}  // namespace docstore::kv
