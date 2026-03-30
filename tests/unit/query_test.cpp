#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>

#include "doc/catalog.h"
#include "doc/key_codec.h"
#include "doc/matcher.h"
#include "doc/planner.h"
#include "util/random.h"

namespace docstore::doc {
namespace {

using json = nlohmann::json;

std::string Enc(const json& v) {
  std::string out;
  EXPECT_TRUE(EncodeIndexValue(v, &out)) << v.dump();
  return out;
}

TEST(KeyCodecTest, NumberOrderMatchesValueOrder) {
  std::vector<double> values = {-std::numeric_limits<double>::infinity(),
                                -DBL_MAX,
                                -1e300,
                                -2.5,
                                -1,
                                -DBL_MIN,
                                -4.9e-324,
                                0.0,
                                4.9e-324,
                                DBL_MIN,
                                1,
                                2.5,
                                1e300,
                                DBL_MAX,
                                std::numeric_limits<double>::infinity()};
  Random rnd(7);
  for (int i = 0; i < 2000; i++) {
    double sign = rnd.OneIn(2) ? -1 : 1;
    values.push_back(sign * std::ldexp(static_cast<double>(rnd.Next() >> 11),
                                       static_cast<int>(rnd.Uniform(200)) - 100));
    values.push_back(static_cast<double>(static_cast<int64_t>(rnd.Next() % 2000000) - 1000000));
  }
  for (size_t i = 0; i < values.size(); i++) {
    for (size_t j = 0; j < values.size(); j += 37) {
      double a = values[i], b = values[(i + j) % values.size()];
      std::string ea = Enc(a), eb = Enc(b);
      ASSERT_EQ(a < b, ea < eb) << a << " vs " << b;
      ASSERT_EQ(a == b, ea == eb) << a << " vs " << b;
    }
  }
  EXPECT_EQ(Enc(-0.0), Enc(0.0));
  EXPECT_EQ(Enc(5), Enc(5.0));  // integers and doubles compare as doubles
  std::string nan;
  EXPECT_FALSE(EncodeIndexValue(std::nan(""), &nan));
}

TEST(KeyCodecTest, StringOrderWithEmbeddedNulsAndTypeOrder) {
  std::vector<std::string> strings = {"",
                                      std::string("\0", 1),
                                      std::string("\0\0", 2),
                                      std::string("\0\x01", 2),
                                      "\x01",
                                      "a",
                                      std::string("a\0", 2),
                                      std::string("a\0b", 3),
                                      "a\x01",
                                      "ab",
                                      "b",
                                      "\xff",
                                      "\xff\xff"};
  std::sort(strings.begin(), strings.end());
  for (size_t i = 0; i + 1 < strings.size(); i++) {
    ASSERT_LT(Enc(strings[i]), Enc(strings[i + 1])) << i;
  }
  // numbers < strings < booleans, and false < true
  EXPECT_LT(Enc(DBL_MAX), Enc(""));
  EXPECT_LT(Enc("\xff\xff"), Enc(false));
  EXPECT_LT(Enc(false), Enc(true));
  for (const json& bad : {json(nullptr), json::array(), json::object()}) {
    std::string out;
    EXPECT_FALSE(EncodeIndexValue(bad, &out));
  }
}

TEST(KeyCodecTest, DecodeRoundTripAndSelfDelimiting) {
  std::vector<json> values = {-3.5, 0, 1e300, "", std::string("x\0y", 3), "plain", true, false};
  std::string all;
  for (const auto& v : values) all += Enc(v);
  all += "trailing-id";
  std::string_view in = all;
  for (const auto& v : values) {
    json got;
    ASSERT_TRUE(DecodeIndexValue(&in, &got));
    EXPECT_EQ(got, v);
  }
  EXPECT_EQ(in, "trailing-id");  // the _id after the value is untouched
  std::string_view bad = "\x20unterminated";
  json got;
  EXPECT_FALSE(DecodeIndexValue(&bad, &got));
}

Filter Parse(const char* text) {
  Filter f;
  Status s = ParseFilter(json::parse(text), &f);
  EXPECT_TRUE(s.ok()) << text << " " << s.ToString();
  return f;
}

TEST(MatcherTest, Semantics) {
  json doc = json::parse(R"({"_id": "1", "age": 21, "score": 7.5, "name": "Asha", "ok": true,
                             "addr": {"city": "Pune"}, "tags": ["x"], "n": null})");
  auto m = [&](const char* f) { return Matches(Parse(f), doc); };
  EXPECT_TRUE(m(R"({})"));
  EXPECT_TRUE(m(R"({"age": 21})"));
  EXPECT_TRUE(m(R"({"age": 21.0})"));   // numbers compare as doubles
  EXPECT_FALSE(m(R"({"age": "21"})"));  // a string never equals a number
  EXPECT_TRUE(m(R"({"age": {"$gte": 18, "$lt": 30}})"));
  EXPECT_FALSE(m(R"({"age": {"$gt": 21}})"));
  EXPECT_FALSE(m(R"({"age": {"$gt": "a"}})"));  // range matches only the bound's type
  EXPECT_TRUE(m(R"({"name": {"$gt": "A", "$lte": "Asha"}})"));
  EXPECT_TRUE(m(R"({"name": {"$in": ["Ben", "Asha"]}})"));
  EXPECT_FALSE(m(R"({"name": {"$in": []}})"));
  EXPECT_TRUE(m(R"({"ok": true})"));
  EXPECT_FALSE(m(R"({"ok": 1})"));
  EXPECT_TRUE(m(R"({"addr.city": "Pune", "age": 21})"));
  EXPECT_FALSE(m(R"({"addr.city": "Pune", "age": 22})"));
  EXPECT_FALSE(m(R"({"missing": 1})"));
  EXPECT_FALSE(m(R"({"tags": {"$gte": ""}})"));  // arrays are not scalars
  EXPECT_FALSE(m(R"({"tags.0": "x"})"));
  EXPECT_FALSE(m(R"({"n": {"$gte": 0}})"));
}

TEST(MatcherTest, InvalidFilters) {
  for (const char* bad : {R"([])", R"({"a": null})", R"({"a": [1]})", R"({"a": {"b": 1}})",
                          R"({"a": {}})", R"({"a": {"$ne": 1}})", R"({"$and": []})",
                          R"({"a": {"$in": 5}})", R"({"a": {"$in": [null]}})",
                          R"({"a": {"$gt": true}})", R"({"a": {"$eq": [1]}})", R"({"a..b": 1})"}) {
    Filter f;
    EXPECT_TRUE(ParseFilter(json::parse(bad), &f).IsInvalidArgument()) << bad;
  }
}

TEST(PlannerTest, PriorityTieBreakAndBounds) {
  CollectionInfo coll{
      "c", 1, {{"age", 2, true}, {"city", 3, true}, {"name", 4, true}, {"zip", 5, false}}};
  auto plan = [&](const char* f) { return ChoosePlan(coll, Parse(f)); };
  EXPECT_TRUE(plan(R"({"other": 1})").collscan);
  EXPECT_TRUE(plan(R"({"zip": 1})").collscan);  // building indexes are never used
  EXPECT_EQ(plan(R"({"age": {"$gt": 1}, "name": "x"})").field, "name");  // $eq beats range
  EXPECT_EQ(plan(R"({"age": {"$in": [1]}, "name": {"$lt": "x"}})").field,
            "age");                                            // $in beats range
  EXPECT_EQ(plan(R"({"name": 1, "city": 2})").field, "city");  // tie: smallest field name
  Plan in = plan(R"({"age": {"$in": [3, 1, 3, 2]}})");
  ASSERT_EQ(in.ranges.size(), 3u);  // distinct, sorted
  EXPECT_EQ(in.ranges[0].display, "[1, 1]");
  EXPECT_EQ(plan(R"({"age": {"$gte": 18, "$lt": 30}})").ranges[0].display, "[18, 30)");
  EXPECT_EQ(plan(R"({"age": {"$gt": 18, "$gte": 18}})").ranges[0].display, "(18, inf.0]");
  EXPECT_EQ(plan(R"({"name": {"$lte": "m"}})").ranges[0].display, "[\"\", \"m\"]");
  json d = DescribePlan(plan(R"({"age": 21})"), json::parse(R"({"age": 21})"));
  EXPECT_EQ(d["winningPlan"]["stage"], "FETCH");
  EXPECT_EQ(d["winningPlan"]["inputStage"]["indexName"], "age_1");
  EXPECT_EQ(d["winningPlan"]["inputStage"]["indexBounds"]["age"][0], "[21, 21]");
}

}  // namespace
}  // namespace docstore::doc
