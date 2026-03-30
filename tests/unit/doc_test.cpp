#include <gtest/gtest.h>

#include <set>
#include <thread>

#include "doc/bson_codec.h"
#include "doc/catalog.h"
#include "doc/field_path.h"
#include "doc/keys.h"
#include "doc/objectid.h"
#include "doc/update_ops.h"

namespace docstore::doc {
namespace {

using json = nlohmann::json;

TEST(ObjectIdTest, FormatAndMonotonicity) {
  std::string prev;
  for (int i = 0; i < 10000; i++) {
    std::string id = GenerateObjectId();
    ASSERT_EQ(id.size(), 24u);
    for (char c : id) ASSERT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << id;
    if (!prev.empty()) {
      ASSERT_LT(prev, id);  // hex order == numeric order
    }
    prev = id;
  }
  // The first 8 hex digits are the current Unix time in seconds, big-endian.
  uint64_t secs = std::stoull(prev.substr(0, 8), nullptr, 16);
  uint64_t now = static_cast<uint64_t>(std::time(nullptr));
  EXPECT_LE(secs, now);
  EXPECT_GE(secs + 5, now);
  // Unique across threads.
  std::set<std::string> ids;
  std::mutex mu;
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&] {
      for (int i = 0; i < 1000; i++) {
        std::string id = GenerateObjectId();
        std::lock_guard<std::mutex> l(mu);
        ids.insert(id);
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(ids.size(), 4000u);
}

TEST(FieldPathTest, SplitAndLookup) {
  std::vector<std::string> seg;
  ASSERT_TRUE(SplitPath("a.b.c", &seg).ok());
  EXPECT_EQ(seg, (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_FALSE(SplitPath("a..b", &seg).ok());
  EXPECT_FALSE(SplitPath("", &seg).ok());
  EXPECT_FALSE(SplitPath("a.$b", &seg).ok());

  json doc = json::parse(R"({"a": {"b": {"c": 5}}, "arr": [{"x": 1}], "n": null})");
  ASSERT_NE(LookupPath(doc, "a.b.c"), nullptr);
  EXPECT_EQ(*LookupPath(doc, "a.b.c"), 5);
  EXPECT_EQ(LookupPath(doc, "a.b.d"), nullptr);
  EXPECT_EQ(LookupPath(doc, "a.b.c.d"), nullptr);  // through a number
  EXPECT_EQ(LookupPath(doc, "arr.x"), nullptr);    // through an array: not a scalar
  ASSERT_NE(LookupPath(doc, "arr"), nullptr);
  EXPECT_TRUE(LookupPath(doc, "arr")->is_array());
  EXPECT_TRUE(LookupPath(doc, "n")->is_null());
}

TEST(UpdateOpsTest, SetUnsetInc) {
  json doc = json::parse(R"({"_id": "1", "a": 1, "b": {"c": 2}, "f": 1.5})");
  ASSERT_TRUE(ApplyUpdate(json::parse(R"({"$set": {"x.y.z": "new", "b.c": 3}})"), &doc).ok());
  EXPECT_EQ(doc["x"]["y"]["z"], "new");
  EXPECT_EQ(doc["b"]["c"], 3);
  ASSERT_TRUE(ApplyUpdate(json::parse(R"({"$unset": {"a": "", "missing.deep": ""}})"), &doc).ok());
  EXPECT_FALSE(doc.contains("a"));
  ASSERT_TRUE(
      ApplyUpdate(json::parse(R"({"$inc": {"b.c": 10, "counter": 2, "f": 1}})"), &doc).ok());
  EXPECT_EQ(doc["b"]["c"], 13);
  EXPECT_TRUE(doc["b"]["c"].is_number_integer());
  EXPECT_EQ(doc["counter"], 2);  // missing field counts as 0
  EXPECT_DOUBLE_EQ(doc["f"].get<double>(), 2.5);
  json big = json::parse(R"({"n": 9223372036854775807})");
  ASSERT_TRUE(ApplyUpdate(json::parse(R"({"$inc": {"n": 1}})"), &big).ok());
  EXPECT_TRUE(big["n"].is_number_float());  // int64 overflow falls back to double
}

TEST(UpdateOpsTest, RejectsBadUpdates) {
  json doc = json::parse(R"({"_id": "1", "s": "text", "o": 5})");
  for (const char* bad : {R"({})", R"({"$push": {"a": 1}})", R"({"a": 1})", R"({"$set": {}})",
                          R"({"$inc": {"s": 1}})", R"({"$inc": {"o": "x"}})",
                          R"({"$set": {"o.x": 1}})", R"({"$set": {"a": 1}, "$inc": {"a": 1}})",
                          R"({"$set": {"a": 1, "a.b": 2}})", R"({"$set": {"a..b": 1}})"}) {
    json copy = doc;
    EXPECT_TRUE(ApplyUpdate(json::parse(bad), &copy).IsInvalidArgument()) << bad;
  }
}

TEST(BsonCodecTest, ValidationRules) {
  EXPECT_TRUE(ValidateDocument(json::parse(R"({"_id": "a", "x": {"y": [1, 2]}})")).ok());
  EXPECT_FALSE(ValidateDocument(json::parse(R"([1])")).ok());
  EXPECT_FALSE(ValidateDocument(json::parse(R"({"x": 1})")).ok());  // no _id
  EXPECT_FALSE(ValidateDocument(json::parse(R"({"_id": 5})")).ok());
  EXPECT_FALSE(ValidateDocument(json::parse(R"({"_id": ""})")).ok());
  EXPECT_FALSE(ValidateDocument(json{{"_id", std::string(513, 'x')}}).ok());
  EXPECT_TRUE(ValidateDocument(json{{"_id", std::string(512, 'x')}}).ok());
  EXPECT_FALSE(ValidateDocument(json::parse(R"({"_id": "a", "$bad": 1})")).ok());
  EXPECT_FALSE(ValidateDocument(json::parse(R"({"_id": "a", "x": {"a.b": 1}})")).ok());
  json deep = 1;
  for (int i = 0; i < 100; i++) deep = json{{"d", deep}};
  deep["_id"] = "a";
  EXPECT_FALSE(ValidateDocument(deep).ok());  // 101 levels
}

TEST(BsonCodecTest, RoundTripAndLimits) {
  json doc = json::parse(R"({"_id": "k", "i": -5, "big": 9007199254740993, "d": 2.5,
                             "s": "x\u0000y", "b": true, "n": null, "o": {"a": [1, "two"]}})");
  std::string bson;
  ASSERT_TRUE(EncodeDocument(doc, &bson).ok());
  json back;
  ASSERT_TRUE(DecodeDocument(bson, &back).ok());
  EXPECT_EQ(back, doc);
  EXPECT_TRUE(EncodeDocument(json::parse(R"({"_id": "a", "u": 18446744073709551615})"), &bson)
                  .IsInvalidArgument());  // no BSON type for uint64 above INT64_MAX
  EXPECT_TRUE(DecodeDocument("garbage", &back).IsCorruption());
  json huge{{"_id", "h"}, {"blob", std::string(17 << 20, 'x')}};
  EXPECT_TRUE(EncodeDocument(huge, &bson).IsInvalidArgument());
}

TEST(CatalogTest, SerializeParseAndLiveness) {
  Catalog c;
  c.next_coll_id = 4;
  c.next_index_id = 7;
  c.collections["users"] = {"users", 1, {{"age", 2, true}, {"city", 5, false}}};
  c.collections["orders"] = {"orders", 3, {}};
  Catalog back;
  ASSERT_TRUE(Catalog::Parse(c.Serialize(), &back).ok());
  EXPECT_EQ(back.Serialize(), c.Serialize());
  EXPECT_FALSE(back.Find("users")->FindIndex("city")->ready);
  EXPECT_TRUE(Catalog::Parse("{oops", &back).IsCorruption());

  EXPECT_FALSE(c.CollectionIsDead(1));
  EXPECT_TRUE(c.CollectionIsDead(2));   // allocated, then dropped
  EXPECT_FALSE(c.CollectionIsDead(4));  // not allocated yet: may be committing right now
  EXPECT_FALSE(c.IndexIsDead(1, 2));
  EXPECT_TRUE(c.IndexIsDead(1, 4));
  EXPECT_TRUE(c.IndexIsDead(2, 2));  // index of a dead collection
  EXPECT_FALSE(c.IndexIsDead(1, 9));

  auto holder = std::make_shared<CatalogHolder>();
  holder->Set(std::make_shared<const Catalog>(c));
  CatalogCompactionFilter filter(holder);
  EXPECT_TRUE(filter.ShouldDrop(DocKey(2, "x")));
  EXPECT_FALSE(filter.ShouldDrop(DocKey(1, "x")));
  EXPECT_TRUE(filter.ShouldDrop(IndexPrefix(1, 4) + "v"));
  EXPECT_FALSE(filter.ShouldDrop(CatalogKey()));
}

TEST(KeysTest, PrefixEndAndCollectionNames) {
  EXPECT_EQ(PrefixEnd("ab"), "ac");
  EXPECT_EQ(PrefixEnd(std::string("a\xff\xff", 3)), "b");
  EXPECT_EQ(PrefixEnd(std::string("\xff", 1)), "");
  EXPECT_LT(DocKey(255, "zzz"), PrefixEnd(CollectionPrefix(255)));
  // The bound must exclude every key of collection 256 (and include none of 255's past it).
  EXPECT_LE(PrefixEnd(CollectionPrefix(255)), DocKey(256, ""));
  EXPECT_TRUE(ValidateCollectionName("users_2").ok());
  EXPECT_FALSE(ValidateCollectionName("").ok());
  EXPECT_FALSE(ValidateCollectionName("bad-name").ok());
  EXPECT_FALSE(ValidateCollectionName(std::string(65, 'a')).ok());
}

}  // namespace
}  // namespace docstore::doc
