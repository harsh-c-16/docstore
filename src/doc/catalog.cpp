#include "doc/catalog.h"

#include "doc/keys.h"
#include "util/coding.h"

namespace docstore::doc {

const IndexInfo* CollectionInfo::FindIndex(std::string_view field) const {
  for (const auto& idx : indexes) {
    if (idx.field == field) return &idx;
  }
  return nullptr;
}

const CollectionInfo* Catalog::Find(std::string_view name) const {
  auto it = collections.find(std::string(name));
  return it == collections.end() ? nullptr : &it->second;
}

std::string Catalog::Serialize() const {
  nlohmann::json j;
  j["next_coll_id"] = next_coll_id;
  j["next_index_id"] = next_index_id;
  j["collections"] = nlohmann::json::array();
  for (const auto& [name, c] : collections) {
    nlohmann::json cj{{"name", name}, {"id", c.id}, {"indexes", nlohmann::json::array()}};
    for (const auto& idx : c.indexes) {
      cj["indexes"].push_back(
          {{"field", idx.field}, {"id", idx.id}, {"state", idx.ready ? "ready" : "building"}});
    }
    j["collections"].push_back(std::move(cj));
  }
  return j.dump();
}

Status Catalog::Parse(std::string_view text, Catalog* out) {
  try {
    nlohmann::json j = nlohmann::json::parse(text);
    Catalog c;
    c.next_coll_id = j.at("next_coll_id").get<uint32_t>();
    c.next_index_id = j.at("next_index_id").get<uint32_t>();
    for (const auto& cj : j.at("collections")) {
      CollectionInfo info;
      info.name = cj.at("name").get<std::string>();
      info.id = cj.at("id").get<uint32_t>();
      for (const auto& ij : cj.at("indexes")) {
        info.indexes.push_back({ij.at("field").get<std::string>(), ij.at("id").get<uint32_t>(),
                                ij.at("state").get<std::string>() == "ready"});
      }
      c.collections[info.name] = std::move(info);
    }
    *out = std::move(c);
    return Status::OK();
  } catch (const nlohmann::json::exception& e) {
    return Status::Corruption(std::string("bad catalog: ") + e.what());
  }
}

bool Catalog::CollectionIsDead(uint32_t coll_id) const {
  // An id at or above next_coll_id may belong to a batch that is being committed right now.
  if (coll_id >= next_coll_id) return false;
  for (const auto& [name, c] : collections) {
    if (c.id == coll_id) return false;
  }
  return true;
}

bool Catalog::IndexIsDead(uint32_t coll_id, uint32_t index_id) const {
  if (CollectionIsDead(coll_id)) return true;
  if (index_id >= next_index_id) return false;
  for (const auto& [name, c] : collections) {
    if (c.id != coll_id) continue;
    for (const auto& idx : c.indexes) {
      if (idx.id == index_id) return false;
    }
  }
  return true;
}

bool CatalogCompactionFilter::ShouldDrop(std::string_view key) const {
  if (key.empty()) return false;
  std::shared_ptr<const Catalog> catalog = holder_->Get();
  if (key[0] == kDocTag && key.size() >= 5) {
    return catalog->CollectionIsDead(DecodeBigEndian32(key.data() + 1));
  }
  if (key[0] == kIndexTag && key.size() >= 9) {
    return catalog->IndexIsDead(DecodeBigEndian32(key.data() + 1),
                                DecodeBigEndian32(key.data() + 5));
  }
  return false;  // the catalog key itself
}

Status ValidateCollectionName(std::string_view name) {
  if (name.empty() || name.size() > 64) {
    return Status::InvalidArgument("collection name must be 1-64 characters");
  }
  for (char ch : name) {
    bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_';
    if (!ok) return Status::InvalidArgument("collection name may only use [A-Za-z0-9_]");
  }
  return Status::OK();
}

}  // namespace docstore::doc
