// Randomized test of the document layer against a std::map model: inserts, upserts, updates,
// deletes, collection creates/drops, flushes, compactions and reopens over a small value domain.
// The seed and op index are printed on failure (rerun with DOCSTORE_TEST_SEED=<seed>).

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <set>

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
    indexed_options_ = options_;
    indexed_options_.env = &indexed_env_;
    Reopen();
  }

  // Two databases receive identical writes; only `indexed_` ever gets indexes, so every query
  // runs once with and once without them.
  void Reopen() {
    db_.reset();
    indexed_.reset();
    Status s = Database::Open(options_, "/db", &db_);
    ASSERT_TRUE(s.ok()) << s.ToString();
    s = Database::Open(indexed_options_, "/db", &indexed_);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  std::string Coll() { return rnd_.OneIn(2) ? "users" : "orders"; }
  std::string Id() { return "id" + std::to_string(rnd_.Uniform(30)); }

  json RandomDoc(const std::string& id) {
    static const char* kNames[] = {"asha", "ben", "chen", "dana", ""};
    static const char* kCities[] = {"pune", "delhi", "goa"};
    json d{{"_id", id}};
    if (!rnd_.OneIn(5))
      d["age"] = rnd_.OneIn(4) ? json(rnd_.Uniform(10) + 0.5) : json(rnd_.Uniform(10));
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
      if (!with_id) doc["_id"] = "gen" + std::to_string(i);  // same id in both databases
      std::string got_id;
      Status s = Both([&](Database* d) { return d->Insert(coll, doc, WriteOptions(), &got_id); });
      if (with_id && model_[coll].count(id)) {
        ASSERT_TRUE(s.IsDuplicateKey()) << s.ToString();
      } else {
        ASSERT_TRUE(s.ok()) << s.ToString();
        model_[coll][got_id] = doc;
      }
    } else if (r < 40) {
      json doc = RandomDoc(Id());
      ASSERT_TRUE(Both([&](Database* d) { return d->Upsert(coll, doc, WriteOptions()); }).ok());
      model_[coll][doc["_id"].get<std::string>()] = doc;
    } else if (r < 60) {
      Update(coll, Id());
    } else if (r < 70) {
      std::string id = Id();
      Status s = Both([&](Database* d) { return d->Delete(coll, id, WriteOptions()); });
      if (model_.count(coll) && model_[coll].erase(id)) {
        ASSERT_TRUE(s.ok()) << s.ToString();
      } else {
        ASSERT_TRUE(s.IsNotFound()) << s.ToString();
      }
    } else if (r < 78) {
      CheckGet(coll, Id());
    } else if (r < 90) {
      CheckFind(coll);
    } else if (r < 93) {
      ToggleIndex(coll);
    } else if (r < 94) {
      Status s = Both([&](Database* d) { return d->DropCollection(coll); });
      ASSERT_EQ(s.ok(), model_.erase(coll) == 1) << s.ToString();
    } else if (r < 95) {
      Status s = Both([&](Database* d) { return d->CreateCollection(coll); });
      ASSERT_EQ(s.ok(), !model_.count(coll)) << s.ToString();
      model_[coll];
    } else if (r < 96) {
      ASSERT_TRUE(Both([](Database* d) { return d->Flush(); }).ok());
    } else if (r < 97) {
      ASSERT_TRUE(Both([](Database* d) { return d->CompactAll(); }).ok());
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
    Status s = Both([&](Database* d) { return d->Update(coll, id, update, WriteOptions()); });
    if (!exists) {
      ASSERT_TRUE(s.IsNotFound()) << s.ToString();
    } else {
      ASSERT_TRUE(s.ok()) << update.dump() << " " << s.ToString();
      model_[coll][id] = expected;
    }
  }

  // Runs a write on both databases; both must return the same status code.
  template <typename F>
  Status Both(F&& op) {
    Status a = op(db_.get());
    Status b = op(indexed_.get());
    EXPECT_EQ(a.code(), b.code()) << a.ToString() << " vs " << b.ToString();
    return a;
  }

  void CheckGet(const std::string& coll, const std::string& id) {
    for (Database* d : {db_.get(), indexed_.get()}) {
      json got;
      Status s = d->Get(coll, id, &got);
      if (model_.count(coll) && model_[coll].count(id)) {
        ASSERT_TRUE(s.ok()) << coll << "/" << id << " " << s.ToString();
        ASSERT_EQ(got, model_[coll][id]);
      } else {
        ASSERT_TRUE(s.IsNotFound()) << coll << "/" << id;
      }
    }
  }

  void ToggleIndex(const std::string& coll) {
    static const char* kFields[] = {"age", "name", "active", "addr.city", "tags"};
    std::string field = kFields[rnd_.Uniform(5)];
    if (rnd_.OneIn(3)) {
      Status s = indexed_->DropIndex(coll, field);
      ASSERT_TRUE(s.ok() || s.IsNotFound()) << s.ToString();
    } else {
      bool existed = model_.count(coll);
      ASSERT_TRUE(indexed_->CreateIndex(coll, field).ok());
      if (!existed) {  // CreateIndex creates the collection; mirror that in the plain db
        ASSERT_TRUE(db_->CreateCollection(coll).ok());
        model_[coll];
      }
    }
  }

  json RandomScalar(const std::string& field) {
    if (field == "age")
      return rnd_.OneIn(4) ? json(rnd_.Uniform(10) + 0.5) : json(rnd_.Uniform(10));
    if (field == "active") return rnd_.OneIn(2);
    if (field == "addr.city")
      return std::vector<std::string>{"pune", "delhi", "goa", "x"}[rnd_.Uniform(4)];
    if (field == "tags") return "a";
    return std::vector<std::string>{"asha", "ben", "chen", "dana", "", "n1"}[rnd_.Uniform(6)];
  }

  json RandomCondition(const std::string& field) {
    switch (rnd_.Uniform(5)) {
      case 0:
        return RandomScalar(field);
      case 1:
        return {{"$in", {RandomScalar(field), RandomScalar(field), RandomScalar(field)}}};
      case 2: {
        json lo = RandomScalar(field), hi = RandomScalar(field);
        if (lo.is_boolean()) return lo;  // booleans have no ranges
        return {{rnd_.OneIn(2) ? "$gte" : "$gt", lo}, {rnd_.OneIn(2) ? "$lt" : "$lte", hi}};
      }
      case 3: {
        json b = RandomScalar(field);
        if (b.is_boolean()) return {{"$eq", b}};
        // Sometimes a bound of the "wrong" type: must match nothing, with or without index.
        if (rnd_.OneIn(4)) b = b.is_string() ? json(3) : json("m");
        return {{std::vector<std::string>{"$gt", "$gte", "$lt", "$lte"}[rnd_.Uniform(4)], b}};
      }
      default: {
        json v = RandomScalar(field);
        if (v.is_boolean()) return {{"$eq", v}, {"$in", {v}}};
        return {{"$eq", v}, {"$gte", v}};
      }
    }
  }

  // Independent re-statement of the matching rules, used as the oracle.
  static bool OracleMatches(const json& filter, const json& doc) {
    auto lookup = [](const json& d, const std::string& path) -> const json* {
      const json* cur = &d;
      size_t start = 0;
      while (true) {
        size_t dot = path.find('.', start);
        std::string k = path.substr(start, dot == std::string::npos ? dot : dot - start);
        if (!cur->is_object() || !cur->contains(k)) return nullptr;
        cur = &(*cur)[k];
        if (dot == std::string::npos) return cur;
        start = dot + 1;
      }
    };
    auto kind = [](const json& v) {
      return v.is_number() ? 0 : v.is_string() ? 1 : v.is_boolean() ? 2 : 3;
    };
    auto cmp = [&](const json& a, const json& b, int* out) {
      if (kind(a) != kind(b) || kind(a) == 3) return false;
      if (kind(a) == 0)
        *out = a.get<double>() < b.get<double>() ? -1 : a.get<double>() > b.get<double>();
      else if (kind(a) == 1)
        *out = a.get<std::string>().compare(b.get<std::string>());
      else
        *out = a.get<bool>() == b.get<bool>() ? 0 : 1;
      if (kind(a) == 1) *out = *out < 0 ? -1 : *out > 0;
      return true;
    };
    for (const auto& [path, cond] : filter.items()) {
      const json* v = lookup(doc, path);
      if (v == nullptr || kind(*v) == 3) return false;
      json ops = cond.is_object() ? cond : json{{"$eq", cond}};
      for (const auto& [op, arg] : ops.items()) {
        int r = 0;
        bool ok;
        if (op == "$in") {
          ok = false;
          for (const auto& a : arg) ok = ok || (cmp(*v, a, &r) && r == 0);
        } else {
          ok = cmp(*v, arg, &r) && (op == "$eq"    ? r == 0
                                    : op == "$gt"  ? r > 0
                                    : op == "$gte" ? r >= 0
                                    : op == "$lt"  ? r < 0
                                                   : r <= 0);
        }
        if (!ok) return false;
      }
    }
    return true;
  }

  void CheckFind(const std::string& coll) {
    static const char* kFields[] = {"age", "name", "active", "addr.city", "tags"};
    json filter = json::object();
    int n = static_cast<int>(rnd_.Uniform(3));  // 0, 1 or 2 fields
    for (int k = 0; k < n; k++) {
      std::string field = kFields[rnd_.Uniform(5)];
      filter[field] = RandomCondition(field);
    }
    std::set<std::string> want;
    if (model_.count(coll)) {
      for (const auto& [id, doc] : model_[coll]) {
        if (OracleMatches(filter, doc)) want.insert(id);
      }
    }
    std::vector<json> plain, with_index;
    json explain;
    ASSERT_TRUE(db_->Find(coll, filter, FindOptions(), &plain).ok()) << filter.dump();
    ASSERT_TRUE(indexed_->Find(coll, filter, FindOptions(), &with_index, &explain).ok());
    auto ids = [&](const std::vector<json>& docs) {
      std::set<std::string> out;
      for (const auto& d : docs) {
        out.insert(d["_id"].get<std::string>());
        EXPECT_EQ(d, model_[coll][d["_id"].get<std::string>()]);
      }
      EXPECT_EQ(out.size(), docs.size()) << "duplicate results";
      return out;
    };
    ASSERT_EQ(ids(plain), want) << "collscan vs oracle, filter " << filter.dump();
    ASSERT_EQ(ids(with_index), want) << "with index vs oracle, filter " << filter.dump() << " plan "
                                     << explain["queryPlanner"].dump();
    ixscans_ += explain["queryPlanner"]["winningPlan"]["stage"] == "FETCH";
    finds_++;
    // A limited query returns a subset of the right size.
    size_t limit = 1 + rnd_.Uniform(3);
    std::vector<json> limited;
    ASSERT_TRUE(indexed_->Find(coll, filter, FindOptions{limit}, &limited).ok());
    ASSERT_EQ(limited.size(), std::min(limit, want.size()));
    for (const auto& d : limited) ASSERT_TRUE(want.count(d["_id"].get<std::string>()));
  }

  int finds_ = 0;
  int ixscans_ = 0;

  void VerifyAll() {
    std::vector<std::string> colls, want;
    ASSERT_TRUE(db_->ListCollections(&colls).ok());
    std::vector<std::string> indexed_colls;
    ASSERT_TRUE(indexed_->ListCollections(&indexed_colls).ok());
    ASSERT_EQ(colls, indexed_colls);
    for (const auto& [name, docs] : model_) want.push_back(name);
    ASSERT_EQ(colls, want);
    for (const auto& [coll, docs] : model_) {
      for (int k = 0; k < 30; k++) CheckGet(coll, "id" + std::to_string(k));
      for (const auto& [id, doc] : docs) CheckGet(coll, id);  // includes generated ObjectIds
    }
  }

 private:
  Random rnd_;
  MemEnv env_, indexed_env_;
  Options options_, indexed_options_;
  std::unique_ptr<Database> db_, indexed_;
  std::map<std::string, std::map<std::string, json>> model_;
};

TEST(DocModelTest, RandomDocumentOpsMatchModel) {
  const int seeds = test::LongMode() ? 50 : 10;
  const int ops = test::LongMode() ? 20000 : 3000;
  const uint64_t base = test::TestSeed();
  int total_finds = 0, total_ixscans = 0;
  for (int s = 0; s < seeds; s++) {
    const uint64_t seed = base + s;
    SCOPED_TRACE("seed " + std::to_string(seed) +
                 " (rerun with DOCSTORE_TEST_SEED=" + std::to_string(seed) + ")");
    DocModel model(seed);
    for (int i = 0; i < ops && !::testing::Test::HasFatalFailure(); i++) model.Step(i);
    if (::testing::Test::HasFatalFailure()) return;
    model.Reopen();
    model.VerifyAll();
    if (::testing::Test::HasFatalFailure()) return;
    total_finds += model.finds_;
    total_ixscans += model.ixscans_;
  }
  std::printf(
      "doc-model: %d finds compared with and without indexes (%d used an index), 0 mismatches\n",
      total_finds, total_ixscans);
  EXPECT_GT(total_ixscans, total_finds / 10);  // the indexed side really used indexes
}

}  // namespace
}  // namespace docstore
