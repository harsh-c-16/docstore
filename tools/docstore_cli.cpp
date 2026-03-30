// docstore_cli <db_path> [--exec "<command>"]
//
// An interactive shell. Each line is either a raw JSON request ({"op": "find", ...}) or a
// shorthand command:
//   insert <coll> <json>        upsert <coll> <json>       update <coll> <id> <json>
//   get <coll> <id>             delete <coll> <id>         find <coll> [json] [limit]
//   explain <coll> [json]       index <coll> <field>       dropindex <coll> <field>
//   indexes <coll>              create <coll>              drop <coll>
//   collections   stats   flush   compact   ping   help   exit
// --exec runs one command and exits with status 0 if it succeeded.

#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "docstore/database.h"
#include "server/command_handler.h"

namespace {

using json = nlohmann::json;

const char* kHelp = R"(commands:
  insert <coll> <json>      upsert <coll> <json>      update <coll> <id> <json>
  get <coll> <id>           delete <coll> <id>        find <coll> [json] [limit]
  explain <coll> [json]     index <coll> <field>      dropindex <coll> <field>
  indexes <coll>            create <coll>             drop <coll>
  collections  stats  flush  compact  ping  help  exit
or a raw JSON request such as {"op": "find", "coll": "users", "filter": {}})";

// Splits off the first whitespace-separated word of *rest.
std::string Word(std::string* rest) {
  size_t b = rest->find_first_not_of(" \t");
  if (b == std::string::npos) {
    rest->clear();
    return "";
  }
  size_t e = rest->find_first_of(" \t", b);
  std::string w = rest->substr(b, e == std::string::npos ? std::string::npos : e - b);
  *rest = e == std::string::npos ? "" : rest->substr(e);
  return w;
}

json ParseJson(const std::string& text, bool* ok) {
  json j = json::parse(text, nullptr, false);
  *ok = !j.is_discarded();
  return j;
}

// Turns a shorthand line into a request; sets *error for malformed input.
json ToRequest(const std::string& line, std::string* error) {
  std::string rest = line;
  const std::string cmd = Word(&rest);
  auto need = [&](const std::string& what) {
    std::string w = Word(&rest);
    if (w.empty() && error->empty()) *error = cmd + ": missing " + what;
    return w;
  };
  auto body = [&](bool required) {
    size_t b = rest.find_first_not_of(" \t");
    std::string text = b == std::string::npos ? "" : rest.substr(b);
    if (text.empty()) {
      if (required && error->empty()) *error = cmd + ": missing JSON argument";
      return json::object();
    }
    bool ok;
    json j = ParseJson(text, &ok);
    if (!ok && error->empty()) *error = cmd + ": argument is not valid JSON: " + text;
    return j;
  };
  if (cmd == "collections" || cmd == "list") return {{"op", "listCollections"}};
  if (cmd == "stats" || cmd == "flush" || cmd == "compact" || cmd == "ping") return {{"op", cmd}};
  if (cmd == "create") return {{"op", "createCollection"}, {"coll", need("collection")}};
  if (cmd == "drop") return {{"op", "dropCollection"}, {"coll", need("collection")}};
  if (cmd == "indexes") return {{"op", "listIndexes"}, {"coll", need("collection")}};
  if (cmd == "index" || cmd == "dropindex") {
    std::string coll = need("collection");
    return {{"op", cmd == "index" ? "createIndex" : "dropIndex"},
            {"coll", coll},
            {"field", need("field")}};
  }
  if (cmd == "insert" || cmd == "upsert") {
    std::string coll = need("collection");
    return {{"op", cmd}, {"coll", coll}, {"doc", body(true)}};
  }
  if (cmd == "get" || cmd == "delete") {
    std::string coll = need("collection");
    return {{"op", cmd}, {"coll", coll}, {"id", need("id")}};
  }
  if (cmd == "update") {
    std::string coll = need("collection");
    std::string id = need("id");
    return {{"op", "update"}, {"coll", coll}, {"id", id}, {"update", body(true)}};
  }
  if (cmd == "find" || cmd == "explain") {
    std::string coll = need("collection");
    // find <coll> [json] [limit]: a trailing all-digit word is the limit.
    size_t limit = 0;
    size_t end = rest.find_last_not_of(" \t");
    size_t begin = end == std::string::npos ? std::string::npos : rest.find_last_of(" \t", end);
    if (cmd == "find" && begin != std::string::npos) {
      std::string tail = rest.substr(begin + 1, end - begin);
      if (tail.find_first_not_of("0123456789") == std::string::npos) {
        limit = std::stoul(tail);
        rest.resize(begin);
      }
    }
    json req{{"op", cmd}, {"coll", coll}, {"filter", body(false)}};
    if (limit) req["limit"] = limit;
    return req;
  }
  *error = "unknown command '" + cmd + "' (try 'help')";
  return json();
}

// Returns true if the command succeeded.
bool RunLine(docstore::CommandHandler* handler, const std::string& line) {
  size_t b = line.find_first_not_of(" \t");
  if (b == std::string::npos) return true;
  json response;
  if (line[b] == '{') {
    bool ok;
    json req = ParseJson(line.substr(b), &ok);
    response = ok ? handler->Handle(req)
                  : docstore::ErrorResponse(docstore::Status::InvalidArgument("not valid JSON"));
  } else {
    std::string error;
    json req = ToRequest(line.substr(b), &error);
    response = error.empty() ? handler->Handle(req)
                             : docstore::ErrorResponse(docstore::Status::InvalidArgument(error));
  }
  std::cout << response.dump(2, ' ', false, json::error_handler_t::replace) << std::endl;
  return response.value("ok", 0) == 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <db_path> [--exec \"<command>\"]\n", argv[0]);
    return 2;
  }
  std::unique_ptr<docstore::Database> db;
  docstore::Status s = docstore::Database::Open(docstore::Options(), argv[1], &db);
  if (!s.ok()) {
    std::fprintf(stderr, "cannot open %s: %s\n", argv[1], s.ToString().c_str());
    return 1;
  }
  docstore::CommandHandler handler(db.get());
  if (argc >= 4 && std::string(argv[2]) == "--exec") return RunLine(&handler, argv[3]) ? 0 : 1;

  const bool interactive = ::isatty(STDIN_FILENO);
  if (interactive) std::cout << "DocStore shell on " << argv[1] << ". Type 'help'.\n";
  std::string line;
  while (true) {
    if (interactive) std::cout << "docstore> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    std::string trimmed = line;
    std::string cmd = Word(&trimmed);
    if (cmd == "exit" || cmd == "quit") break;
    if (cmd == "help") {
      std::cout << kHelp << std::endl;
      continue;
    }
    RunLine(&handler, line);
  }
  return db->Close().ok() ? 0 : 1;
}
