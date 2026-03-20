#include "server/command_handler.h"

namespace docstore {

using json = nlohmann::json;

json ErrorResponse(const Status& s) {
  return {{"ok", 0}, {"code", std::string(s.CodeName())}, {"errmsg", s.message()}};
}

namespace {

json Ok(json extra = json::object()) {
  extra["ok"] = 1;
  return extra;
}

json FromStatus(const Status& s, json extra = json::object()) {
  return s.ok() ? Ok(std::move(extra)) : ErrorResponse(s);
}

// Throws json::exception (caught in Handle) when the field is missing or has the wrong type.
std::string Str(const json& req, const char* field) { return req.at(field).get<std::string>(); }

WriteOptions WriteOpts(const json& req) {
  return WriteOptions{.sync = req.value("sync", false)};
}

}  // namespace

json CommandHandler::Handle(const json& req) {
  try {
    if (!req.is_object() || !req.contains("op") || !req["op"].is_string()) {
      return ErrorResponse(Status::InvalidArgument("request needs a string field 'op'"));
    }
    const std::string op = req["op"];
    if (op == "ping") return Ok();
    if (op == "listCollections") {
      std::vector<std::string> names;
      Status s = db_->ListCollections(&names);
      return FromStatus(s, {{"collections", names}});
    }
    if (op == "createCollection") return FromStatus(db_->CreateCollection(Str(req, "coll")));
    if (op == "dropCollection") return FromStatus(db_->DropCollection(Str(req, "coll")));
    if (op == "insert") {
      std::string id;
      Status s = db_->Insert(Str(req, "coll"), req.at("doc"), WriteOpts(req), &id);
      return FromStatus(s, {{"insertedId", id}});
    }
    if (op == "upsert") return FromStatus(db_->Upsert(Str(req, "coll"), req.at("doc"), WriteOpts(req)));
    if (op == "update") {
      return FromStatus(
          db_->Update(Str(req, "coll"), Str(req, "id"), req.at("update"), WriteOpts(req)));
    }
    if (op == "delete") return FromStatus(db_->Delete(Str(req, "coll"), Str(req, "id"), WriteOpts(req)));
    if (op == "get") {
      json doc;
      Status s = db_->Get(Str(req, "coll"), Str(req, "id"), &doc);
      return FromStatus(s, {{"doc", doc}});
    }
    if (op == "find") {
      std::vector<json> docs;
      FindOptions fo{req.value("limit", size_t{0})};
      Status s = db_->Find(Str(req, "coll"), req.value("filter", json::object()), fo, &docs);
      return FromStatus(s, {{"docs", docs}});
    }
    if (op == "explain") {
      json explain;
      Status s = db_->Explain(Str(req, "coll"), req.value("filter", json::object()), &explain);
      return FromStatus(s, {{"explain", explain}});
    }
    if (op == "createIndex") {
      const std::string coll = Str(req, "coll"), field = Str(req, "field");
      return FromStatus(db_->CreateIndex(coll, field), {{"indexName", field + "_1"}});
    }
    if (op == "dropIndex") return FromStatus(db_->DropIndex(Str(req, "coll"), Str(req, "field")));
    if (op == "listIndexes") {
      std::vector<std::string> fields;
      Status s = db_->ListIndexes(Str(req, "coll"), &fields);
      return FromStatus(s, {{"indexes", fields}});
    }
    if (op == "stats") return Ok({{"stats", db_->Stats()}});
    if (op == "flush") return FromStatus(db_->Flush());
    if (op == "compact") return FromStatus(db_->CompactAll());
    return ErrorResponse(Status::InvalidArgument("unknown op '" + op + "'"));
  } catch (const json::exception& e) {
    // Missing fields or wrong types in the request: a client error, never a crash.
    return ErrorResponse(Status::InvalidArgument(std::string("bad request: ") + e.what()));
  }
}

std::string CommandHandler::HandleLine(std::string_view line) {
  json req = json::parse(line.begin(), line.end(), nullptr, /*allow_exceptions=*/false);
  if (req.is_discarded()) return ErrorResponse(Status::InvalidArgument("request is not valid JSON")).dump();
  // error_handler_t::replace: invalid UTF-8 inside strings must not throw while serializing.
  return Handle(req).dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace docstore
