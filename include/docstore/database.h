#pragma once

// DocStore's public API: a single-node document database over the kv::Engine LSM tree.
// No exceptions cross this API; every error is a Status.

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "docstore/options.h"
#include "docstore/status.h"

namespace docstore {

namespace kv {
class Engine;
class WriteBatch;
}  // namespace kv
namespace doc {
struct Catalog;
struct CollectionInfo;
class CatalogHolder;
}  // namespace doc

class Database {
 public:
  static Status Open(const Options& options, const std::string& path,
                     std::unique_ptr<Database>* out);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Status Close();  // also called by the destructor

  Status CreateCollection(const std::string& coll);  // InvalidArgument if it exists
  Status DropCollection(const std::string& coll);    // NotFound if it does not
  Status ListCollections(std::vector<std::string>* out);

  // DuplicateKey if _id exists. Generates an ObjectId _id if the document has none.
  Status Insert(const std::string& coll, nlohmann::json doc, const WriteOptions& options,
                std::string* id_out);
  // Requires _id; inserts or replaces.
  Status Upsert(const std::string& coll, nlohmann::json doc, const WriteOptions& options);
  // $set / $unset / $inc; NotFound if the document is absent.
  Status Update(const std::string& coll, const std::string& id, const nlohmann::json& update,
                const WriteOptions& options);
  Status Delete(const std::string& coll, const std::string& id, const WriteOptions& options);
  Status Get(const std::string& coll, const std::string& id, nlohmann::json* out);

  Status Flush();       // force memtable -> SSTable
  Status CompactAll();  // force a full compaction (also reclaims dropped collections)
  nlohmann::json Stats();

 private:
  Database();

  std::shared_ptr<const doc::Catalog> CurrentCatalog() const;
  // Commits the batch; if new_catalog is set, it is written in the same batch and installed in
  // memory only after the batch committed. Caller holds write_mutex_.
  Status Commit(kv::WriteBatch* batch, std::shared_ptr<const doc::Catalog> new_catalog,
                const WriteOptions& options);
  // Reads the stored document (caller holds write_mutex_, so it cannot change underneath).
  Status ReadDoc(uint32_t coll_id, const std::string& id, nlohmann::json* out);
  // The shared write path of Insert/Upsert/Update/Delete. new_doc == nullptr deletes.
  Status WriteDoc(const doc::CollectionInfo& coll, const std::string& id,
                  const nlohmann::json* old_doc, const nlohmann::json* new_doc,
                  std::shared_ptr<const doc::Catalog> new_catalog, const WriteOptions& options);

  std::unique_ptr<kv::Engine> engine_;
  std::shared_ptr<doc::CatalogHolder> catalog_;
  // Serializes document writes, including their read-modify-write of the old document and its
  // index entries. Taken before the engine's own write mutex (DESIGN.md, lock order).
  std::mutex write_mutex_;
};

}  // namespace docstore
