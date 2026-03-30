// Power-loss tests: random workloads with mixed sync flags on a FaultInjectionEnv, a simulated
// power cut at a random point (including inside flush, compaction and recovery), then a reopen.
// The recovered state must equal the state after some prefix of the attempted writes, and that
// prefix must include every acknowledged sync=true write (the prefix property).

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "env/fault_injection_env.h"
#include "kv/engine.h"
#include "test_util.h"
#include "util/random.h"

namespace docstore::kv {
namespace {

using State = std::map<std::string, std::string>;

struct Op {
  std::vector<std::pair<std::string, std::string>> puts;  // empty value = delete
  bool sync = false;
};

void Apply(const Op& op, State* state) {
  for (const auto& [k, v] : op.puts) {
    if (v.empty()) {
      state->erase(k);
    } else {
      (*state)[k] = v;
    }
  }
}

State ReadAll(Engine* engine) {
  std::vector<std::pair<std::string, std::string>> rows;
  Status s = engine->Scan(ReadOptions(), 0, &rows);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return State(rows.begin(), rows.end());
}

// Returns "" if `recovered` equals the state after k attempted ops for some k >= min_k.
std::string CheckPrefix(const std::vector<Op>& attempted, size_t min_k, const State& recovered) {
  State state;
  for (size_t k = 0; k <= attempted.size(); k++) {
    if (k > 0) Apply(attempted[k - 1], &state);
    if (k >= min_k && state == recovered) return "";
  }
  return "recovered state (" + std::to_string(recovered.size()) +
         " keys) matches no prefix of length >= " + std::to_string(min_k) + " of " +
         std::to_string(attempted.size()) + " attempted ops";
}

class PowerLossTest : public ::testing::Test {};

TEST_F(PowerLossTest, PrefixPropertyHoldsAfterRandomPowerCuts) {
  const int iterations = test::LongMode() ? 500 : 50;
  const uint64_t base_seed = test::TestSeed();
  int cuts_in_background = 0;
  int total_ops = 0;
  for (int iter = 0; iter < iterations; iter++) {
    const uint64_t seed = base_seed + iter;
    SCOPED_TRACE("seed " + std::to_string(seed) +
                 " (rerun with DOCSTORE_TEST_SEED=" + std::to_string(seed) + ")");
    Random rnd(seed);
    FaultInjectionEnv env;
    Options options;
    options.env = &env;
    options.memtable_size = 2 << 10;  // flush every few dozen writes
    options.target_file_size = 4 << 10;
    options.l0_compaction_trigger = 3;
    options.journal_commit_interval_ms = rnd.OneIn(2) ? 1 : 0;
    // Cut the power at the n-th hook point (flush, compaction or recovery) or, if the hooks
    // are not reached in time, at a random op from the test thread.
    std::atomic<int> hooks_left{1 << 30};  // armed after the first open
    std::atomic<bool> cut_in_background{false};
    options.test_hook = [&](std::string_view) {
      if (hooks_left.fetch_sub(1) == 0) {
        env.SimulatePowerLoss();
        cut_in_background = true;
      }
    };
    const int cut_at_op = 50 + static_cast<int>(rnd.Uniform(400));

    std::unique_ptr<Engine> engine;
    ASSERT_TRUE(Engine::Open(options, "/db", &engine).ok());
    hooks_left = static_cast<int>(rnd.Uniform(40));
    std::vector<Op> attempted;
    size_t min_k = 0;  // every op up to the last acknowledged sync=true write must survive
    for (int i = 0; i < cut_at_op && !env.powered_off(); i++) {
      Op op;
      op.sync = rnd.OneIn(5);
      WriteBatch batch;
      int n = 1 + static_cast<int>(rnd.Uniform(3));
      for (int j = 0; j < n; j++) {
        std::string key = "k" + std::to_string(rnd.Uniform(40));
        if (rnd.OneIn(4)) {
          batch.Delete(key);
          op.puts.emplace_back(key, "");
        } else {
          std::string value =
              "v" + std::to_string(i) + "." + std::to_string(j) + std::string(rnd.Uniform(60), 'x');
          batch.Put(key, value);
          op.puts.emplace_back(key, value);
        }
      }
      attempted.push_back(op);  // an op that returns an error may or may not have survived
      Status s = engine->Write(WriteOptions{.sync = op.sync}, &batch);
      if (!s.ok()) break;
      if (op.sync) min_k = attempted.size();
      if (rnd.OneIn(60)) (void)engine->Flush();
      if (rnd.OneIn(150)) (void)engine->CompactAll();
    }
    if (!env.powered_off()) env.SimulatePowerLoss();
    cuts_in_background += cut_in_background;
    total_ops += static_cast<int>(attempted.size());
    engine.reset();  // the "dead" process: every write it tries now fails
    env.PowerOn();

    // Sometimes crash again in the middle of recovery itself.
    hooks_left = rnd.OneIn(4) ? 0 : 1 << 30;
    Status s = Engine::Open(options, "/db", &engine);
    if (env.powered_off()) {
      engine.reset();
      env.PowerOn();
      s = Engine::Open(options, "/db", &engine);
    }
    ASSERT_TRUE(s.ok()) << s.ToString();
    hooks_left = 1 << 30;
    std::string problem = CheckPrefix(attempted, min_k, ReadAll(engine.get()));
    ASSERT_EQ(problem, "");
    // The recovered database must keep working.
    ASSERT_TRUE(engine->Put(WriteOptions{.sync = true}, "after", "crash").ok());
  }
  RecordProperty("iterations", iterations);
  RecordProperty("cuts_inside_background_work", cuts_in_background);
  RecordProperty("total_ops", total_ops);
  std::printf("power-loss: %d iterations, %d ops, %d cuts inside flush/compaction, 0 violations\n",
              iterations, total_ops, cuts_in_background);
  EXPECT_GT(cuts_in_background, 0);  // the hooks really fired inside flush/compaction
}

}  // namespace
}  // namespace docstore::kv
