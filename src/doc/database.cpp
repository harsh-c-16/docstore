#include "docstore/database.h"

#include "doc/bson_codec.h"
#include "doc/catalog.h"
#include "doc/keys.h"
#include "doc/objectid.h"
#include "doc/update_ops.h"
#include "kv/engine.h"

namespace docstore {

using doc::Catalog;
using doc::CollectionInfo;
using json = nlohmann::json;

Database::Database() : catalog_(std::make_shared<doc::CatalogHolder>()) {}

Database::~Database() { (void)Close(); }

Status Database::Open(const Options& options, const std::string& path,
                      std::unique_ptr<Database>* out) {
  std::unique_ptr<Database> db(new Database());
  Options engine_options = options;
  // The filter reads the live catalog. Before the catalog is loaded it sees an empty catalog
  // whose next ids are 1, so it treats every id as possibly live and drops nothing.
  engine_options.compaction_filter = std::make_shared<doc::CatalogCompactionFilter>(db->catalog_);
  Status s = kv::Engine::Open(engine_options, path, &db->engine_);
  if (!s.ok()) return s;
  std::string text;
  s = db->engine_->Get(doc::CatalogKey(), &text);
  auto catalog = std::make_shared<Catalog>();
  if (s.ok()) {
    s = Catalog::Parse(text, catalog.get());
  } else if (s.IsNotFound()) {
    s = Status::OK();  // new database
  }
  if (!s.ok()) return s;
  db->catalog_->Set(std::move(catalog));
  *out = std::move(db);
  return Status::OK();
}

Status Database::Close() {
  if (!engine_) return Status::OK();
  std::lock_guard<std::mutex> w(write_mutex_);
  return engine_->Close();
}

std::shared_ptr<const Catalog> Database::CurrentCatalog() const { return catalog_->Get(); }

Status Database::Commit(kv::WriteBatch* batch, std::shared_ptr<const Catalog> new_catalog,
                        const WriteOptions& options) {
  if (new_catalog) batch->Put(doc::CatalogKey(), new_catalog->Serialize());
  Status s = engine_->Write(options, batch);
  // Installed only after the commit: the compaction filter must never see a drop that is not
  // durable in the WAL yet (drops are written with sync=true, DESIGN.md D23).
  if (s.ok() && new_catalog) catalog_->Set(std::move(new_catalog));
  return s;
}

Status Database::ReadDoc(uint32_t coll_id, const std::string& id, json* out) {
  std::string bson;
  Status s = engine_->Get(doc::DocKey(coll_id, id), &bson);
  if (!s.ok()) return s;
  return doc::DecodeDocument(bson, out);
}

Status Database::WriteDoc(const CollectionInfo& coll, const std::string& id, const json* old_doc,
                          const json* new_doc, std::shared_ptr<const Catalog> new_catalog,
                          const WriteOptions& options) {
  (void)old_doc;  // index maintenance will use it
  kv::WriteBatch batch;
  const std::string key = doc::DocKey(coll.id, id);
  if (new_doc == nullptr) {
    batch.Delete(key);
  } else {
    std::string bson;
    Status s = doc::EncodeDocument(*new_doc, &bson);
    if (!s.ok()) return s;
    batch.Put(key, bson);
  }
  return Commit(&batch, std::move(new_catalog), options);
}

// ---------------------------------------------------------------- collections

Status Database::CreateCollection(const std::string& coll) {
  Status s = doc::ValidateCollectionName(coll);
  if (!s.ok()) return s;
  std::lock_guard<std::mutex> w(write_mutex_);
  auto catalog = CurrentCatalog();
  if (catalog->Find(coll)) return Status::InvalidArgument("collection already exists: " + coll);
  auto next = std::make_shared<Catalog>(*catalog);
  next->collections[coll] = CollectionInfo{coll, next->next_coll_id++, {}};
  kv::WriteBatch batch;
  return Commit(&batch, std::move(next), WriteOptions());
}

Status Database::DropCollection(const std::string& coll) {
  std::lock_guard<std::mutex> w(write_mutex_);
  auto catalog = CurrentCatalog();
  if (!catalog->Find(coll)) return Status::NotFound("no collection " + coll);
  // O(1): only the catalog entry goes. The documents become unreachable at once (lookups use
  // live ids only) and compaction reclaims them later through the CompactionFilter.
  auto next = std::make_shared<Catalog>(*catalog);
  next->collections.erase(coll);
  kv::WriteBatch batch;
  return Commit(&batch, std::move(next), WriteOptions{.sync = true});
}

Status Database::ListCollections(std::vector<std::string>* out) {
  out->clear();
  for (const auto& [name, c] : CurrentCatalog()->collections) out->push_back(name);
  return Status::OK();
}

// ---------------------------------------------------------------- documents

Status Database::Insert(const std::string& coll, json doc, const WriteOptions& options,
                        std::string* id_out) {
  Status s = doc::ValidateCollectionName(coll);
  if (!s.ok()) return s;
  if (!doc.is_object()) return Status::InvalidArgument("document must be a JSON object");
  if (!doc.contains("_id")) doc["_id"] = doc::GenerateObjectId();
  s = doc::ValidateDocument(doc);
  if (!s.ok()) return s;
  const std::string id = doc["_id"].get<std::string>();

  std::lock_guard<std::mutex> w(write_mutex_);
  auto catalog = CurrentCatalog();
  std::shared_ptr<Catalog> next;
  const CollectionInfo* info = catalog->Find(coll);
  if (info == nullptr) {
    // Implicit creation, in the same batch as the first document.
    next = std::make_shared<Catalog>(*catalog);
    info = &(next->collections[coll] = CollectionInfo{coll, next->next_coll_id++, {}});
  } else {
    json existing;
    s = ReadDoc(info->id, id, &existing);
    if (s.ok()) return Status::DuplicateKey("duplicate _id " + id + " in " + coll);
    if (!s.IsNotFound()) return s;
  }
  s = WriteDoc(*info, id, nullptr, &doc, std::move(next), options);
  if (s.ok() && id_out != nullptr) *id_out = id;
  return s;
}

Status Database::Upsert(const std::string& coll, json doc, const WriteOptions& options) {
  Status s = doc::ValidateCollectionName(coll);
  if (s.ok()) s = doc::ValidateDocument(doc);  // requires _id
  if (!s.ok()) return s;
  const std::string id = doc["_id"].get<std::string>();

  std::lock_guard<std::mutex> w(write_mutex_);
  auto catalog = CurrentCatalog();
  std::shared_ptr<Catalog> next;
  const CollectionInfo* info = catalog->Find(coll);
  json old_doc;
  bool had_old = false;
  if (info == nullptr) {
    next = std::make_shared<Catalog>(*catalog);
    info = &(next->collections[coll] = CollectionInfo{coll, next->next_coll_id++, {}});
  } else {
    s = ReadDoc(info->id, id, &old_doc);
    if (!s.ok() && !s.IsNotFound()) return s;
    had_old = s.ok();
  }
  return WriteDoc(*info, id, had_old ? &old_doc : nullptr, &doc, std::move(next), options);
}

Status Database::Update(const std::string& coll, const std::string& id, const json& update,
                        const WriteOptions& options) {
  std::lock_guard<std::mutex> w(write_mutex_);
  const auto catalog = CurrentCatalog();
  const CollectionInfo* info = catalog->Find(coll);
  if (info == nullptr) return Status::NotFound("no collection " + coll);
  json old_doc;
  Status s = ReadDoc(info->id, id, &old_doc);
  if (!s.ok()) return s;
  json new_doc = old_doc;
  s = doc::ApplyUpdate(update, &new_doc);
  if (!s.ok()) return s;
  if (!new_doc.contains("_id") || new_doc["_id"] != old_doc["_id"]) {
    return Status::InvalidArgument("an update may not change _id");
  }
  s = doc::ValidateDocument(new_doc);
  if (!s.ok()) return s;
  return WriteDoc(*info, id, &old_doc, &new_doc, nullptr, options);
}

Status Database::Delete(const std::string& coll, const std::string& id,
                        const WriteOptions& options) {
  std::lock_guard<std::mutex> w(write_mutex_);
  const auto catalog = CurrentCatalog();
  const CollectionInfo* info = catalog->Find(coll);
  if (info == nullptr) return Status::NotFound("no collection " + coll);
  json old_doc;
  Status s = ReadDoc(info->id, id, &old_doc);
  if (!s.ok()) return s;
  return WriteDoc(*info, id, &old_doc, nullptr, nullptr, options);
}

Status Database::Get(const std::string& coll, const std::string& id, json* out) {
  Status s = doc::ValidateId(id);
  if (!s.ok()) return s;
  const auto catalog = CurrentCatalog();
  const CollectionInfo* info = catalog->Find(coll);
  if (info == nullptr) return Status::NotFound("no collection " + coll);
  return ReadDoc(info->id, id, out);
}

// ---------------------------------------------------------------- maintenance

Status Database::Flush() { return engine_->Flush(); }

Status Database::CompactAll() { return engine_->CompactAll(); }

json Database::Stats() {
  json j = engine_->Stats();
  j["collections"] = CurrentCatalog()->collections.size();
  return j;
}

}  // namespace docstore
