#include "doc/index_manager.h"

#include "doc/bson_codec.h"
#include "doc/field_path.h"
#include "doc/key_codec.h"
#include "doc/keys.h"
#include "docstore/database.h"
#include "kv/engine.h"
#include "util/logger.h"

namespace docstore {
namespace doc {
namespace {

// enc(value at field) or "" if the document has no indexable value there.
std::string EncodedField(const nlohmann::json* doc, const std::string& field) {
  std::string enc;
  if (doc == nullptr) return enc;
  const nlohmann::json* v = LookupPath(*doc, field);
  if (v == nullptr || !EncodeIndexValue(*v, &enc)) enc.clear();
  return enc;
}

}  // namespace

void UpdateIndexEntries(const CollectionInfo& coll, const std::string& id,
                        const nlohmann::json* old_doc, const nlohmann::json* new_doc,
                        kv::WriteBatch* batch) {
  for (const auto& idx : coll.indexes) {
    if (!idx.ready) continue;  // "building" indexes only exist after a crash; rebuilt on open
    const std::string old_enc = EncodedField(old_doc, idx.field);
    const std::string new_enc = EncodedField(new_doc, idx.field);
    if (old_enc == new_enc) continue;
    const std::string prefix = IndexPrefix(coll.id, idx.id);
    if (!old_enc.empty()) batch->Delete(prefix + old_enc + id);
    if (!new_enc.empty()) batch->Put(prefix + new_enc + id, "");
  }
}

}  // namespace doc

using doc::Catalog;
using doc::CollectionInfo;

Status Database::CreateIndex(const std::string& coll, const std::string& field) {
  Status s = doc::ValidateCollectionName(coll);
  std::vector<std::string> segments;
  if (s.ok()) s = doc::SplitPath(field, &segments);
  if (!s.ok()) return s;
  std::lock_guard<std::mutex> w(write_mutex_);
  auto catalog = CurrentCatalog();
  auto next = std::make_shared<Catalog>(*catalog);
  auto it = next->collections.find(coll);
  if (it == next->collections.end()) {
    it = next->collections.emplace(coll, CollectionInfo{coll, next->next_coll_id++, {}}).first;
  } else if (it->second.FindIndex(field) != nullptr) {
    return Status::OK();  // like MongoDB: creating an existing index is a no-op
  }
  // Step 1: a catalog entry in state "building" with a new id. Queries ignore it. It is synced
  // so that, whatever happens during the build, the next open knows to resume it.
  it->second.indexes.push_back({field, next->next_index_id++, false});
  kv::WriteBatch batch;
  s = Commit(&batch, std::move(next), WriteOptions{.sync = true});
  if (!s.ok()) return s;
  return BuildIndex(coll, field);
}

Status Database::BuildIndex(const std::string& coll, const std::string& field) {
  const auto catalog = CurrentCatalog();
  const CollectionInfo* info = catalog->Find(coll);
  const doc::IndexInfo* idx = info->FindIndex(field);
  const std::string index_prefix = doc::IndexPrefix(info->id, idx->id);
  const std::string coll_prefix = doc::CollectionPrefix(info->id);
  // Step 2: scan the collection and write index keys in batches of ~1,000. The write mutex is
  // held, so no document can change during the build (writes are blocked: a documented limit).
  auto it = engine_->GetSnapshot()->NewIterator(
      kv::ReadOptions{coll_prefix, doc::PrefixEnd(coll_prefix)});
  kv::WriteBatch batch;
  for (; it->Valid(); it->Next()) {
    nlohmann::json d;
    Status s = doc::DecodeDocument(it->value(), &d);
    if (!s.ok()) return s;
    const nlohmann::json* v = doc::LookupPath(d, field);
    std::string enc;
    if (v != nullptr && doc::EncodeIndexValue(*v, &enc)) {
      batch.Put(index_prefix + enc + std::string(it->key().substr(coll_prefix.size())), "");
    }
    if (batch.Count() >= 1000) {
      s = engine_->Write(WriteOptions(), &batch);
      if (!s.ok()) return s;
      batch.Clear();
      if (test_hook_) test_hook_("index_build:batch");
    }
  }
  if (!it->status().ok()) return it->status();
  Status s = engine_->Write(WriteOptions(), &batch);
  if (!s.ok()) return s;
  // Step 3: flip to "ready" with sync=true; from now on the planner may use it.
  auto next = std::make_shared<Catalog>(*CurrentCatalog());
  for (auto& i : next->collections[coll].indexes) {
    if (i.field == field) i.ready = true;
  }
  kv::WriteBatch flip;
  return Commit(&flip, std::move(next), WriteOptions{.sync = true});
}

Status Database::ResumeIndexBuilds() {
  std::lock_guard<std::mutex> w(write_mutex_);
  // Hold the catalog we iterate: each rebuild installs a new one, which would free this one if
  // the loop only referred to it through a temporary shared_ptr.
  const auto catalog = CurrentCatalog();
  for (const auto& [name, coll] : catalog->collections) {
    for (const auto& idx : coll.indexes) {
      if (idx.ready) continue;
      // Rebuild from scratch under a new id; the partial entries of the old id are dead and
      // are reclaimed by compaction.
      Log(LogLevel::kInfo, "resuming interrupted build of index " + name + "." + idx.field);
      auto next = std::make_shared<Catalog>(*CurrentCatalog());
      for (auto& i : next->collections[name].indexes) {
        if (i.field == idx.field) i.id = next->next_index_id++;
      }
      kv::WriteBatch batch;
      Status s = Commit(&batch, std::move(next), WriteOptions());
      if (s.ok()) s = BuildIndex(name, idx.field);
      if (!s.ok()) return s;
    }
  }
  return Status::OK();
}

Status Database::DropIndex(const std::string& coll, const std::string& field) {
  std::lock_guard<std::mutex> w(write_mutex_);
  auto catalog = CurrentCatalog();
  const CollectionInfo* info = catalog->Find(coll);
  if (info == nullptr || info->FindIndex(field) == nullptr) {
    return Status::NotFound("no index " + field + "_1 on " + coll);
  }
  auto next = std::make_shared<Catalog>(*catalog);
  auto& indexes = next->collections[coll].indexes;
  std::erase_if(indexes, [&](const doc::IndexInfo& i) { return i.field == field; });
  kv::WriteBatch batch;
  // sync=true for the same reason as DropCollection (DESIGN.md D23).
  return Commit(&batch, std::move(next), WriteOptions{.sync = true});
}

Status Database::ListIndexes(const std::string& coll, std::vector<std::string>* out) {
  out->clear();
  const auto catalog = CurrentCatalog();
  const CollectionInfo* info = catalog->Find(coll);
  if (info == nullptr) return Status::NotFound("no collection " + coll);
  for (const auto& idx : info->indexes) out->push_back(idx.field);
  return Status::OK();
}

}  // namespace docstore
