#include <gtest/gtest.h>

#include <memory>

#include "docstore/database.h"
#include "env/mem_env.h"

namespace docstore {
namespace {

using json = nlohmann::json;

class DatabaseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    options_.env = &env_;
    Open();
  }
  void Open() {
    db_.reset();
    Status s = Database::Open(options_, "/db", &db_);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }
  json Get(const std::string& coll, const std::string& id) {
    json out;
    Status s = db_->Get(coll, id, &out);
    return s.ok() ? out : json(s.CodeName());
  }

  MemEnv env_;
  Options options_;
  std::unique_ptr<Database> db_;
};

TEST_F(DatabaseTest, InsertGetAndGeneratedObjectId) {
  std::string id;
  ASSERT_TRUE(db_->Insert("users", json::parse(R"({"name": "Asha", "age": 21})"), {}, &id).ok());
  EXPECT_EQ(id.size(), 24u);
  json doc = Get("users", id);
  EXPECT_EQ(doc["name"], "Asha");
  EXPECT_EQ(doc["_id"], id);
  ASSERT_TRUE(db_->Insert("users", json::parse(R"({"_id": "u1", "age": 30})"), {}, &id).ok());
  EXPECT_EQ(id, "u1");
  EXPECT_TRUE(db_->Insert("users", json::parse(R"({"_id": "u1"})"), {}, nullptr).IsDuplicateKey());
  EXPECT_EQ(Get("users", "nope"), "NotFound");
  EXPECT_EQ(Get("nocoll", "u1"), "NotFound");
  std::vector<std::string> colls;
  ASSERT_TRUE(db_->ListCollections(&colls).ok());
  EXPECT_EQ(colls, std::vector<std::string>{"users"});  // created implicitly by the insert
}

TEST_F(DatabaseTest, UpsertUpdateDelete) {
  ASSERT_TRUE(db_->Upsert("c", json::parse(R"({"_id": "a", "n": 1})"), {}).ok());
  ASSERT_TRUE(db_->Upsert("c", json::parse(R"({"_id": "a", "m": 2})"), {}).ok());  // replaces
  EXPECT_EQ(Get("c", "a"), json::parse(R"({"_id": "a", "m": 2})"));
  EXPECT_TRUE(db_->Upsert("c", json::parse(R"({"m": 2})"), {}).IsInvalidArgument());  // no _id

  ASSERT_TRUE(
      db_->Update("c", "a", json::parse(R"({"$inc": {"m": 5}, "$set": {"x.y": true}})"), {}).ok());
  EXPECT_EQ(Get("c", "a"), json::parse(R"({"_id": "a", "m": 7, "x": {"y": true}})"));
  EXPECT_TRUE(db_->Update("c", "zz", json::parse(R"({"$set": {"q": 1}})"), {}).IsNotFound());
  EXPECT_TRUE(
      db_->Update("c", "a", json::parse(R"({"$set": {"_id": "b"}})"), {}).IsInvalidArgument());
  EXPECT_TRUE(
      db_->Update("c", "a", json::parse(R"({"$unset": {"_id": ""}})"), {}).IsInvalidArgument());
  EXPECT_TRUE(db_->Update("c", "a", json::parse(R"({"$set": {"$x": 1}})"), {}).IsInvalidArgument());

  ASSERT_TRUE(db_->Delete("c", "a", {}).ok());
  EXPECT_TRUE(db_->Delete("c", "a", {}).IsNotFound());
  EXPECT_EQ(Get("c", "a"), "NotFound");
}

TEST_F(DatabaseTest, ValidationErrors) {
  EXPECT_TRUE(db_->Insert("bad-name", json::object(), {}, nullptr).IsInvalidArgument());
  EXPECT_TRUE(db_->Insert("c", json::array(), {}, nullptr).IsInvalidArgument());
  EXPECT_TRUE(db_->Insert("c", json::parse(R"({"_id": 7})"), {}, nullptr).IsInvalidArgument());
  EXPECT_TRUE(db_->Insert("c", json::parse(R"({"a.b": 7})"), {}, nullptr).IsInvalidArgument());
  EXPECT_TRUE(db_->Insert("c", json::parse(R"({"u": 18446744073709551615})"), {}, nullptr)
                  .IsInvalidArgument());
  json out;
  EXPECT_TRUE(db_->Get("c", "", &out).IsInvalidArgument());
  std::vector<std::string> colls;
  ASSERT_TRUE(db_->ListCollections(&colls).ok());
  EXPECT_TRUE(colls.empty());  // a failed insert does not create its collection
}

TEST_F(DatabaseTest, CollectionsSurviveReopenAndDropIsolatesData) {
  ASSERT_TRUE(db_->CreateCollection("empty").ok());
  EXPECT_TRUE(db_->CreateCollection("empty").IsInvalidArgument());
  ASSERT_TRUE(db_->Insert("users", json::parse(R"({"_id": "u1"})"), {}, nullptr).ok());
  Open();
  std::vector<std::string> colls;
  ASSERT_TRUE(db_->ListCollections(&colls).ok());
  EXPECT_EQ(colls, (std::vector<std::string>{"empty", "users"}));
  ASSERT_TRUE(db_->DropCollection("users").ok());
  EXPECT_TRUE(db_->DropCollection("users").IsNotFound());
  // A new collection with the same name gets a new id: the old documents never reappear.
  ASSERT_TRUE(db_->Insert("users", json::parse(R"({"_id": "u2"})"), {}, nullptr).ok());
  EXPECT_EQ(Get("users", "u1"), "NotFound");
  Open();
  EXPECT_EQ(Get("users", "u1"), "NotFound");
  EXPECT_EQ(Get("users", "u2")["_id"], "u2");
}

TEST_F(DatabaseTest, CompactionReclaimsDroppedCollections) {
  for (int i = 0; i < 2000; i++) {
    json doc{{"_id", "d" + std::to_string(i)}, {"pad", std::string(200, 'p')}};
    ASSERT_TRUE(db_->Insert("big", doc, {}, nullptr).ok());
  }
  ASSERT_TRUE(db_->Insert("small", json::parse(R"({"_id": "keep"})"), {}, nullptr).ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  uint64_t before = db_->Stats()["l1_bytes"].get<uint64_t>();
  ASSERT_TRUE(db_->DropCollection("big").ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  uint64_t after = db_->Stats()["l1_bytes"].get<uint64_t>();
  EXPECT_GT(before, 400000u);
  EXPECT_LT(after, 10000u) << "dropped collection's documents were not reclaimed";
  EXPECT_EQ(Get("small", "keep")["_id"], "keep");
}

}  // namespace
}  // namespace docstore
