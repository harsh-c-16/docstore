#pragma once

// Secondary index maintenance. An index on `field` holds the key
// 0x02 | coll_id | index_id | enc(value) | _id (empty value) for each document whose value at
// `field` is a number, string or boolean. Indexes are sparse: missing, null, array and object
// values are not indexed. Index name: "<field>_1".

#include <string>

#include <nlohmann/json.hpp>

#include "doc/catalog.h"
#include "kv/write_batch.h"

namespace docstore::doc {

// Adds the index changes for replacing old_doc by new_doc (either may be null) to the batch,
// for every ready index of the collection. Unchanged values produce no change.
void UpdateIndexEntries(const CollectionInfo& coll, const std::string& id,
                        const nlohmann::json* old_doc, const nlohmann::json* new_doc,
                        kv::WriteBatch* batch);

}  // namespace docstore::doc
