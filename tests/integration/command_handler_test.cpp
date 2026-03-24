#include <gtest/gtest.h>

#include <cstdio>
#include <memory>

#include "env/mem_env.h"
#include "server/command_handler.h"
#include "test_util.h"

namespace docstore {
namespace {

using json = nlohmann::json;

class CommandHandlerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    options_.env = &env_;
    ASSERT_TRUE(Database::Open(options_, "/db", &db_).ok());
    handler_ = std::make_unique<CommandHandler>(db_.get());
  }
  json Run(const char* request) { return handler_->Handle(json::parse(request)); }

  MemEnv env_;
  Options options_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<CommandHandler> handler_;
};

TEST_F(CommandHandlerTest, EveryOpEndToEnd) {
  EXPECT_EQ(Run(R"({"op": "ping"})")["ok"], 1);
  EXPECT_EQ(Run(R"({"op": "createCollection", "coll": "users"})")["ok"], 1);
  json r = Run(R"({"op": "insert", "coll": "users", "doc": {"name": "Asha", "age": 21}})");
  ASSERT_EQ(r["ok"], 1);
  const std::string id = r["insertedId"];
  EXPECT_EQ(Run(R"({"op": "upsert", "coll": "users", "doc": {"_id": "u2", "age": 30}, "sync": true})")["ok"], 1);
  json u = handler_->Handle({{"op", "update"}, {"coll", "users"}, {"id", id},
                             {"update", {{"$inc", {{"age", 1}}}}}});
  EXPECT_EQ(u["ok"], 1);
  json g = handler_->Handle({{"op", "get"}, {"coll", "users"}, {"id", id}});
  EXPECT_EQ(g["doc"]["age"], 22);
  EXPECT_EQ(Run(R"({"op": "createIndex", "coll": "users", "field": "age"})")["indexName"], "age_1");
  EXPECT_EQ(Run(R"({"op": "listIndexes", "coll": "users"})")["indexes"], json::array({"age"}));
  json f = Run(R"({"op": "find", "coll": "users", "filter": {"age": {"$gte": 22}}, "limit": 10})");
  EXPECT_EQ(f["docs"].size(), 2u);
  json e = Run(R"({"op": "explain", "coll": "users", "filter": {"age": 30}})");
  EXPECT_EQ(e["explain"]["queryPlanner"]["winningPlan"]["inputStage"]["stage"], "IXSCAN");
  EXPECT_EQ(Run(R"({"op": "listCollections"})")["collections"], json::array({"users"}));
  EXPECT_EQ(Run(R"({"op": "flush"})")["ok"], 1);
  EXPECT_EQ(Run(R"({"op": "compact"})")["ok"], 1);
  EXPECT_GE(Run(R"({"op": "stats"})")["stats"]["compactions"].get<int>(), 1);
  EXPECT_EQ(Run(R"({"op": "dropIndex", "coll": "users", "field": "age"})")["ok"], 1);
  EXPECT_EQ(handler_->Handle({{"op", "delete"}, {"coll", "users"}, {"id", id}})["ok"], 1);
  EXPECT_EQ(Run(R"({"op": "dropCollection", "coll": "users"})")["ok"], 1);
}

TEST_F(CommandHandlerTest, ErrorsAreResponsesNotCrashes) {
  auto code = [&](const char* req) { return Run(req)["code"]; };
  EXPECT_EQ(code(R"({"op": "get", "coll": "users", "id": "nope"})"), "NotFound");
  ASSERT_EQ(Run(R"({"op": "insert", "coll": "c", "doc": {"_id": "x"}})")["ok"], 1);
  EXPECT_EQ(code(R"({"op": "insert", "coll": "c", "doc": {"_id": "x"}})"), "DuplicateKey");
  EXPECT_EQ(code(R"({"op": "bogus"})"), "InvalidArgument");
  EXPECT_EQ(code(R"({"no_op": 1})"), "InvalidArgument");
  EXPECT_EQ(code(R"({"op": "get", "coll": "c"})"), "InvalidArgument");          // missing id
  EXPECT_EQ(code(R"({"op": "get", "coll": 5, "id": "x"})"), "InvalidArgument");  // wrong type
  EXPECT_EQ(code(R"({"op": "find", "coll": "c", "filter": {"a": {"$where": 1}}})"), "InvalidArgument");
  json bad = json::parse(handler_->HandleLine("{not json"));
  EXPECT_EQ(bad["ok"], 0);
  EXPECT_EQ(bad["code"], "InvalidArgument");
}

TEST(CliTest, ExecRunsOneCommand) {
  const std::string dir = test::NewTempDir("cli");
  auto run = [&](const std::string& cmd, int* status) {
    std::string full = std::string(DOCSTORE_CLI_PATH) + " " + dir + " --exec '" + cmd + "' 2>&1";
    FILE* p = ::popen(full.c_str(), "r");
    std::string out;
    char buf[4096];
    while (size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    *status = ::pclose(p);
    return out;
  };
  int status = 0;
  std::string out = run(R"(insert users {"_id": "a1", "name": "Asha", "age": 21})", &status);
  EXPECT_EQ(status, 0) << out;
  EXPECT_NE(out.find("\"insertedId\": \"a1\""), std::string::npos) << out;
  out = run("index users age", &status);
  EXPECT_EQ(status, 0) << out;
  out = run(R"(find users {"age": {"$gte": 18}})", &status);
  EXPECT_NE(out.find("Asha"), std::string::npos) << out;
  out = run(R"(explain users {"age": 21})", &status);
  EXPECT_NE(out.find("IXSCAN"), std::string::npos) << out;
  out = run(R"({"op": "get", "coll": "users", "id": "a1"})", &status);
  EXPECT_NE(out.find("Asha"), std::string::npos) << out;
  out = run("get users missing", &status);
  EXPECT_NE(status, 0);
  EXPECT_NE(out.find("NotFound"), std::string::npos) << out;
  out = run("frobnicate", &status);
  EXPECT_NE(out.find("unknown command"), std::string::npos) << out;
}

}  // namespace
}  // namespace docstore
