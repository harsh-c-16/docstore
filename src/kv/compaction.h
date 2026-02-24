#pragma once

// L0 -> L1 compaction of a two-level LSM (DESIGN.md D16).
//
// Inputs: every L0 file plus every L1 file overlapping their combined key range (or, for a
// full compaction, every file). The inputs are merged; for each user key only the newest entry
// survives, tombstones are dropped (L1 is the bottom level and every older version of the key
// is among the inputs), and keys rejected by the CompactionFilter are dropped. Outputs are new
// L1 files cut at target_file_size.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "docstore/options.h"
#include "docstore/status.h"
#include "kv/version_edit.h"

namespace docstore {
class Env;
}

namespace docstore::kv {

struct Version;
class VersionSet;

struct Compaction {
  std::vector<std::shared_ptr<const FileMeta>> inputs[kNumLevels];
  uint64_t InputBytes() const;
  bool empty() const { return inputs[0].empty() && inputs[1].empty(); }
};

// full=true takes every file (CompactAll); otherwise all of L0 and the overlapping L1 files.
Compaction PickCompaction(const Version& version, bool full);

struct CompactionResult {
  std::vector<FileMeta> outputs;  // opened, synced, not yet installed
  uint64_t bytes_written = 0;
  uint64_t entries_dropped = 0;
};

// Writes the merged outputs. `between_entries` runs periodically during the merge (the engine
// uses it to flush a waiting memtable); returning false aborts the compaction. On any failure
// or abort, outputs already written are deleted and *aborted / the status says why.
Status RunCompaction(const Options& options, Env* env, const std::string& dbname,
                     VersionSet* versions, const Compaction& c,
                     const std::function<bool()>& between_entries, CompactionResult* result,
                     bool* aborted);

}  // namespace docstore::kv
