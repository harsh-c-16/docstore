#pragma once

// The catalog: which collections and indexes exist and their numeric ids. Stored as JSON under
// the single key 0x00"catalog" and written in the same WriteBatch as the data it describes.
//   {"next_coll_id": 3, "next_index_id": 5,
//    "collections": [{"name": "users", "id": 1,
//                     "indexes": [{"field": "age", "id": 2, "state": "ready"}]}]}
// Ids are never reused, which is what makes dropping by catalog entry safe (DESIGN.md D23).

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "docstore/options.h"
#include "docstore/status.h"

namespace docstore::doc {

struct IndexInfo {
  std::string field;
  uint32_t id = 0;
  bool ready = false;  // false = "building": not used by queries, rebuilt on open
};

struct CollectionInfo {
  std::string name;
  uint32_t id = 0;
  std::vector<IndexInfo> indexes;

  const IndexInfo* FindIndex(std::string_view field) const;
};

struct Catalog {
  uint32_t next_coll_id = 1;
  uint32_t next_index_id = 1;
  std::map<std::string, CollectionInfo> collections;  // by name

  const CollectionInfo* Find(std::string_view name) const;
  std::string Serialize() const;
  static Status Parse(std::string_view text, Catalog* out);

  // For the compaction filter: true if this id was allocated and is no longer in use.
  bool CollectionIsDead(uint32_t coll_id) const;
  bool IndexIsDead(uint32_t coll_id, uint32_t index_id) const;
};

// The live catalog, shared by readers, the writer and the compaction filter. Readers copy the
// pointer under a shared lock and then use an immutable Catalog without any lock; the writer
// installs a new Catalog (under the database write mutex) only after its batch committed.
class CatalogHolder {
 public:
  std::shared_ptr<const Catalog> Get() const {
    std::shared_lock<std::shared_mutex> l(mu_);
    return catalog_;
  }
  void Set(std::shared_ptr<const Catalog> c) {
    std::unique_lock<std::shared_mutex> l(mu_);
    catalog_ = std::move(c);
  }

 private:
  mutable std::shared_mutex mu_;
  std::shared_ptr<const Catalog> catalog_ = std::make_shared<const Catalog>();
};

// Lets compaction reclaim the keys of dropped collections and indexes (their ids are never
// reused, so those keys can never be written again: the CompactionFilter contract).
class CatalogCompactionFilter final : public CompactionFilter {
 public:
  explicit CatalogCompactionFilter(std::shared_ptr<const CatalogHolder> holder)
      : holder_(std::move(holder)) {}
  bool ShouldDrop(std::string_view user_key) const override;

 private:
  std::shared_ptr<const CatalogHolder> holder_;
};

// Collection names: 1-64 characters of [A-Za-z0-9_].
Status ValidateCollectionName(std::string_view name);

}  // namespace docstore::doc
