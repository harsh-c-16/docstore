#include "doc/executor.h"

#include "doc/bson_codec.h"
#include "doc/key_codec.h"
#include "doc/keys.h"

namespace docstore::doc {

using json = nlohmann::json;

Status Execute(const kv::Snapshot& snapshot, uint32_t coll_id, const Filter& filter,
               const Plan& plan, size_t limit, std::vector<json>* out, ExecutionStats* stats) {
  out->clear();
  *stats = ExecutionStats();
  auto done = [&] { return limit != 0 && out->size() >= limit; };
  auto consider = [&](std::string_view bson) -> Status {
    json doc;
    Status s = DecodeDocument(bson, &doc);
    if (!s.ok()) return s;
    stats->docs_examined++;
    if (Matches(filter, doc)) out->push_back(std::move(doc));
    return Status::OK();
  };

  if (plan.collscan) {
    const std::string prefix = CollectionPrefix(coll_id);
    auto it = snapshot.NewIterator(kv::ReadOptions{prefix, PrefixEnd(prefix)});
    for (; it->Valid() && !done(); it->Next()) {
      Status s = consider(it->value());
      if (!s.ok()) return s;
    }
    stats->returned = out->size();
    return it->status();
  }

  const std::string prefix = IndexPrefix(coll_id, plan.index_id);
  for (const auto& range : plan.ranges) {
    auto it = snapshot.NewIterator(kv::ReadOptions{range.lower, range.upper});
    for (; it->Valid() && !done(); it->Next()) {
      stats->keys_examined++;
      // Key = prefix | enc(value) | _id; the encoding is self-delimiting.
      std::string_view rest = it->key().substr(prefix.size());
      json value;
      if (!DecodeIndexValue(&rest, &value)) return Status::Corruption("bad index key");
      std::string bson;
      Status s = snapshot.Get(DocKey(coll_id, rest), &bson);
      // Same snapshot as the index, so the document must be there.
      if (s.IsNotFound()) return Status::Corruption("index entry points to a missing document");
      if (s.ok()) s = consider(bson);
      if (!s.ok()) return s;
    }
    if (!it->status().ok()) return it->status();
    if (done()) break;
  }
  stats->returned = out->size();
  return Status::OK();
}

}  // namespace docstore::doc
