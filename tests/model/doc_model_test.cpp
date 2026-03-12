// Randomized test of the document layer against a std::map model: inserts, upserts, updates,
// deletes, collection creates/drops, flushes, compactions and reopens over a small value domain.
// The seed and op index are printed on failure (rerun with DOCSTORE_TEST_SEED=<seed>).

#include <gtest/gtest.h>

#include <map>
#include <memory>

#include "docstore/database.h"
#include "env/mem_env.h"
#include "test_util.h"
#include "util/random.h"

namespace docstore {
namespace {

using json = nlohmann::json;

class DocModel {
 public:
  explicit DocModel(uint64_t seed) : rnd_(seed) {
    options_.env = &env_;
    options_.memtable_size = 16 << 10;
    options_.target_file_size = 32 << 10;
    Reopen();
  }

  void Reopen() {
    db_.reset();
    Status s = Database::Open(options_, "/db", &db_);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  std::string Coll() { return rnd_.OneIn(2) ? "users" : "orders"; }
  std::string Id() { return "id" + std::to_string(rnd_.Uniform(30)); }

  json RandomDoc(const std::string& id) {
    static const char* kNames[] = {"asha", "ben", "chen", "dana", ""};
    static const char* kCities[] = {"pune", "delhi", "goa"};
    json d{{"_id", id}};
    if (!rnd_.OneIn(5)) d["age"] = rnd_.OneIn(4) ? json(rnd_.Uniform(10) + 0.5) : json(rnd_.Uniform(10));
    if (!rnd_.OneIn(5)) d["name"] = kNames[rnd_.Uniform(5)];
    if (rnd_.OneIn(2)) d["addr"] = {{"city", kCities[rnd_.Uniform(3)]}};
    if (rnd_.OneIn(4)) d["active"] = rnd_.OneIn(2);
    if (rnd_.OneIn(6)) d["tags"] = json::array({"a", rnd_.Uniform(3)});
    return d;
  }

  void Step(int i) {
    SCOPED_TRACE("op " + std::to_string(i));
    const uint64_t r = rnd_.Uniform(100);
    const std::string coll = Coll();
    if (r < 25) {
      bool with_id = !rnd_.OneIn(5);
      std::string id = with_id ? Id() : "";
      json doc = RandomDoc(id);
      if (!with_id) doc.erase("_id");
      std::string got_id;
      Status s = db_->Insert(coll, doc, WriteOptions(), &got_id);
      if (with_id && model_[coll].count(id)) {
        ASSERT_TRUE(s.IsDuplicateKey()) << s.ToString();
      } else {
        ASSERT_TRUE(s.ok()) << s.ToString();
        if (!with_id) doc["_id"] = got_id;
        model_[coll][got_id] = doc;
      }
    } else if (r < 40) {
      json doc = RandomDoc(Id());
      ASSERT_TRUE(db_->Upsert(coll, doc, WriteOptions()).ok());
      model_[coll][doc["_id"].get<std::string>()] = doc;
    } else if (r < 60) {
      Update(coll, Id());
    } else if (r < 70) {
      std::string id = Id();
      Status s = db_->Delete(coll, id, WriteOptions());
      if (model_.count(coll) && model_[coll].erase(id)) {
        ASSERT_TRUE(s.ok()) << s.ToString();
      } else {
        ASSERT_TRUE(s.IsNotFound()) << s.ToString();
      }
    } else if (r < 94) {
      CheckGet(coll, Id());
    } else if (r < 95) {
      Status s = db_->DropCollection(coll);
      ASSERT_EQ(s.ok(), model_.erase(coll) == 1) << s.ToString();
    } else if (r < 96) {
      Status s = db_->CreateCollection(coll);
      ASSERT_EQ(s.ok(), !model_.count(coll)) << s.ToString();
      model_[coll];
    } else if (r < 97) {
      ASSERT_TRUE(db_->Flush().ok());
    } else if (r < 98) {
      ASSERT_TRUE(db_->CompactAll().ok());
    } else {
      Reopen();
    }
  }

  // A few update shapes whose expected effect the model computes by hand (not with the code
  // under test).
  void Update(const std::string& coll, const std::string& id) {
    bool exists = model_.count(coll) && model_[coll].count(id);
    json expected = exists ? model_[coll][id] : json();
    json update;
    switch (rnd_.Uniform(4)) {
      case 0: {
        std::string name = "n" + std::to_string(rnd_.Uniform(5));
        update = {{"$set", {{"name", name}}}};
        expected["name"] = name;
        break;
      }
      case 1: {
        int64_t by = static_cast<int64_t>(rnd_.Uniform(5)) - 2;
        update = {{"$inc", {{"age", by}}}};
        if (!expected.contains("age")) {
          expected["age"] = by;
        } else if (expected["age"].is_number_integer()) {
          expected["age"] = expected["age"].get<int64_t>() + by;
        } else {
          expected["age"] = expected["age"].get<double>() + static_cast<double>(by);
        }
        break;
      }
      case 2:
        update = {{"$unset", {{"addr.city", ""}}}};
        if (expected.contains("addr")) expected["addr"].erase("city");
        break;
      default: {
        update = {{"$set", {{"addr.city", "x"}}}};
        if (!expected.contains("addr")) expected["addr"] = json::object();
        expected["addr"]["city"] = "x";
      }
    }
    Status s = db_->Update(coll, id, update, WriteOptions());
    if (!exists) {
      ASSERT_TRUE(s.IsNotFound()) << s.ToString();
    } else {
      ASSERT_TRUE(s.ok()) << update.dump() << " " << s.ToString();
      model_[coll][id] = expected;
    }
  }

  void CheckGet(const std::string& coll, const std::string& id) {
    json got;
    Status s = db_->Get(coll, id, &got);
    if (model_.count(coll) && model_[coll].count(id)) {
      ASSERT_TRUE(s.ok()) << coll << "/" << id << " " << s.ToString();
      ASSERT_EQ(got, model_[coll][id]);
    } else {
      ASSERT_TRUE(s.IsNotFound()) << coll << "/" << id;
    }
  }

  void VerifyAll() {
    std::vector<std::string> colls, want;
    ASSERT_TRUE(db_->ListCollections(&colls).ok());
    for (const auto& [name, docs] : model_) want.push_back(name);
    ASSERT_EQ(colls, want);
    for (const auto& [coll, docs] : model_) {
      for (int k = 0; k < 30; k++) CheckGet(coll, "id" + std::to_string(k));
      for (const auto& [id, doc] : docs) CheckGet(coll, id);  // includes generated ObjectIds
    }
  }

 private:
  Random rnd_;
  MemEnv env_;
  Options options_;
  std::unique_ptr<Database> db_;
  std::map<std::string, std::map<std::string, json>> model_;
};

TEST(DocModelTest, RandomDocumentOpsMatchModel) {
  const int seeds = test::LongMode() ? 50 : 10;
  const int ops = test::LongMode() ? 20000 : 3000;
  const uint64_t base = test::TestSeed();
  for (int s = 0; s < seeds; s++) {
    const uint64_t seed = base + s;
    SCOPED_TRACE("seed " + std::to_string(seed) + " (rerun with DOCSTORE_TEST_SEED=" +
                 std::to_string(seed) + ")");
    DocModel model(seed);
    for (int i = 0; i < ops && !::testing::Test::HasFatalFailure(); i++) model.Step(i);
    if (::testing::Test::HasFatalFailure()) return;
    model.Reopen();
    model.VerifyAll();
    if (::testing::Test::HasFatalFailure()) return;
  }
}

}  // namespace
}  // namespace docstore
