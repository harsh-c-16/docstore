#include <gtest/gtest.h>

#include <memory>
#include <set>

#include "docstore/database.h"
#include "env/fault_injection_env.h"

namespace docstore {
namespace {

using json = nlohmann::json;

class FindTest : public ::testing::Test {
 protected:
  void SetUp() override {
    options_.env = &env_;
    Open();
    for (int i = 0; i < 1000; i++) {
      json doc{
          {"_id", "u" + std::to_string(i)}, {"age", i % 100}, {"city", i % 2 ? "Pune" : "Goa"}};
      if (i % 10 == 0) doc.erase("age");       // sparse: no index entry
      if (i % 7 == 0) doc["age"] = "unknown";  // strings sort after numbers
      ASSERT_TRUE(db_->Insert("users", doc, {}, nullptr).ok());
    }
  }
  void Open() {
    db_.reset();
    Status s = Database::Open(options_, "/db", &db_);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }
  std::vector<json> Find(const char* filter, json* explain = nullptr, size_t limit = 0) {
    std::vector<json> out;
    Status s = db_->Find("users", json::parse(filter), FindOptions{limit}, &out, explain);
    EXPECT_TRUE(s.ok()) << s.ToString();
    return out;
  }

  FaultInjectionEnv env_;
  Options options_;
  std::unique_ptr<Database> db_;
};

TEST_F(FindTest, IndexedAndCollectionScanAgreeAndExplainShowsTheDifference) {
  const char* filter = R"({"age": {"$gte": 20, "$lt": 22}, "city": "Pune"})";
  json collscan_explain, ixscan_explain;
  auto without = Find(filter, &collscan_explain);
  ASSERT_TRUE(db_->CreateIndex("users", "age").ok());
  auto with = Find(filter, &ixscan_explain);
  auto ids = [](const std::vector<json>& v) {
    std::vector<std::string> out;
    for (const auto& d : v) out.push_back(d["_id"]);
    std::sort(out.begin(), out.end());
    return out;
  };
  EXPECT_EQ(ids(with), ids(without));
  EXPECT_FALSE(with.empty());
  EXPECT_EQ(collscan_explain["queryPlanner"]["winningPlan"]["stage"], "COLLSCAN");
  EXPECT_EQ(collscan_explain["executionStats"]["totalDocsExamined"], 1000);
  EXPECT_EQ(collscan_explain["executionStats"]["totalKeysExamined"], 0);
  const json& ix = ixscan_explain["queryPlanner"]["winningPlan"];
  EXPECT_EQ(ix["stage"], "FETCH");
  EXPECT_EQ(ix["inputStage"]["stage"], "IXSCAN");
  EXPECT_EQ(ix["inputStage"]["indexBounds"]["age"][0], "[20, 22)");
  // Ages 20 and 21 exist for 20 docs minus the sparse/string ones; only those are touched.
  EXPECT_LE(ixscan_explain["executionStats"]["totalKeysExamined"].get<int>(), 20);
  EXPECT_EQ(ixscan_explain["executionStats"]["totalDocsExamined"],
            ixscan_explain["executionStats"]["totalKeysExamined"]);
  EXPECT_EQ(ixscan_explain["executionStats"]["nReturned"], with.size());
  auto stats = db_->Stats();
  EXPECT_EQ(stats["queries_collscan"], 1);
  EXPECT_EQ(stats["queries_ixscan"], 1);
}

TEST_F(FindTest, IndexIsMaintainedByEveryWriteAndSurvivesReopen) {
  ASSERT_TRUE(db_->CreateIndex("users", "age").ok());
  ASSERT_TRUE(db_->CreateIndex("users", "age").ok());  // idempotent
  ASSERT_TRUE(db_->Update("users", "u1", json::parse(R"({"$set": {"age": 500}})"), {}).ok());
  ASSERT_TRUE(db_->Upsert("users", json::parse(R"({"_id": "u2", "age": 500})"), {}).ok());
  ASSERT_TRUE(db_->Insert("users", json::parse(R"({"_id": "new", "age": 500})"), {}, nullptr).ok());
  ASSERT_TRUE(db_->Delete("users", "u3", {}).ok());
  ASSERT_TRUE(db_->Update("users", "u4", json::parse(R"({"$unset": {"age": ""}})"), {}).ok());
  Open();
  json explain;
  EXPECT_EQ(Find(R"({"age": 500})", &explain).size(), 3u);
  EXPECT_EQ(explain["queryPlanner"]["winningPlan"]["inputStage"]["stage"], "IXSCAN");
  auto ids = [&](const char* filter) {
    std::set<std::string> out;
    for (const auto& d : Find(filter)) out.insert(d["_id"]);
    return out;
  };
  EXPECT_FALSE(ids(R"({"age": 1})").count("u1"));  // moved to 500: old entry removed
  EXPECT_TRUE(ids(R"({"age": 1})").count("u101"));
  EXPECT_FALSE(ids(R"({"age": 3})").count("u3"));  // deleted
  EXPECT_FALSE(ids(R"({"age": 4})").count("u4"));  // field unset: no entry (sparse)
  std::vector<std::string> indexes;
  ASSERT_TRUE(db_->ListIndexes("users", &indexes).ok());
  EXPECT_EQ(indexes, std::vector<std::string>{"age"});
  ASSERT_TRUE(db_->DropIndex("users", "age").ok());
  EXPECT_TRUE(db_->DropIndex("users", "age").IsNotFound());
  Find(R"({"age": 500})", &explain);
  EXPECT_EQ(explain["queryPlanner"]["winningPlan"]["stage"], "COLLSCAN");
}

TEST_F(FindTest, InLimitOrderAndTypeBrackets) {
  ASSERT_TRUE(db_->CreateIndex("users", "age").ok());
  auto r = Find(R"({"age": {"$in": [5, 3, 3]}})");
  ASSERT_FALSE(r.empty());
  for (size_t i = 1; i < r.size(); i++) {  // IXSCAN order: (value, _id)
    ASSERT_LE(r[i - 1]["age"].get<int>(), r[i]["age"].get<int>());
  }
  EXPECT_EQ(Find(R"({"age": {"$gte": 0}})", nullptr, 5).size(), 5u);
  // String values live in their own type bracket of the index.
  auto strings = Find(R"({"age": {"$gte": "a"}})");
  EXPECT_FALSE(strings.empty());
  for (const auto& d : strings) EXPECT_EQ(d["age"], "unknown");
  json bad;
  std::vector<json> out;
  EXPECT_TRUE(
      db_->Find("users", json::parse(R"({"age": {"$ne": 1}})"), {}, &out).IsInvalidArgument());
  EXPECT_TRUE(db_->Find("nocoll", json::object(), {}, &out).ok());
  EXPECT_TRUE(out.empty());
  ASSERT_TRUE(db_->Explain("users", json::parse(R"({"city": "Goa"})"), &bad).ok());
  EXPECT_EQ(bad["executionStats"]["nReturned"], 500);
}

TEST_F(FindTest, InterruptedIndexBuildIsRebuiltOnOpen) {
  for (int i = 1000; i < 5000; i++) {  // enough documents for several build batches
    ASSERT_TRUE(
        db_->Insert("users", json{{"_id", "u" + std::to_string(i)}, {"age", i % 100}}, {}, nullptr)
            .ok());
  }
  ASSERT_TRUE(db_->Flush().ok());
  int batches = 0;
  options_.test_hook = [&](std::string_view point) {
    if (point == "index_build:batch" && ++batches == 2) env_.SimulatePowerLoss();
  };
  Open();
  EXPECT_FALSE(db_->CreateIndex("users", "age").ok());  // the power went out mid-build
  db_.reset();
  env_.PowerOn();
  options_.test_hook = nullptr;
  Open();  // finds the index in state "building" and rebuilds it under a new id
  json explain;
  auto r = Find(R"({"age": 42})", &explain);
  EXPECT_EQ(explain["queryPlanner"]["winningPlan"]["inputStage"]["stage"], "IXSCAN");
  std::vector<json> scan;
  ASSERT_TRUE(db_->DropIndex("users", "age").ok());
  scan = Find(R"({"age": 42})");
  EXPECT_EQ(r.size(), scan.size());
  EXPECT_GT(r.size(), 30u);
}

}  // namespace
}  // namespace docstore
