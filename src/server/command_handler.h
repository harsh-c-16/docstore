#pragma once

// One code path for the CLI and the TCP server: a JSON request with an "op" field in, a JSON
// response out. Success: {"ok": 1, ...}. Failure: {"ok": 0, "code": "DuplicateKey",
// "errmsg": "..."}. Never throws.
//
// Ops: ping, listCollections, createCollection{coll}, dropCollection{coll},
// insert{coll, doc, sync?}, upsert{coll, doc, sync?}, update{coll, id, update, sync?},
// delete{coll, id, sync?}, get{coll, id}, find{coll, filter, limit?}, explain{coll, filter},
// createIndex{coll, field}, dropIndex{coll, field}, listIndexes{coll}, stats, flush, compact.

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "docstore/database.h"

namespace docstore {

class CommandHandler {
 public:
  explicit CommandHandler(Database* db) : db_(db) {}

  nlohmann::json Handle(const nlohmann::json& request);
  // Parses one request line; malformed JSON becomes an InvalidArgument response.
  std::string HandleLine(std::string_view line);

 private:
  Database* db_;
};

nlohmann::json ErrorResponse(const Status& s);

}  // namespace docstore
