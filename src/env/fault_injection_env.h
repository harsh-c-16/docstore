#pragma once

// Test-only filesystem that proves fsync correctness instead of assuming it.
//
// It keeps two namespaces: the visible (volatile) one in MemEnv::files_, and a durable one that
// only changes on SyncDir. Each file's durable contents are the prefix that was Sync()ed.
// SimulatePowerLoss() throws away everything that was not made durable, exactly the worst case
// POSIX allows: unsynced appends vanish, and creates/renames/deletes revert unless the
// directory was synced.

#include <atomic>

#include "env/mem_env.h"

namespace docstore {

class FaultInjectionEnv final : public MemEnv {
 public:
  // Reverts to the durable state and "powers off": every later mutation fails with IOError
  // until PowerOn(), so a still-running engine cannot write into the post-crash state.
  void SimulatePowerLoss();
  void PowerOn() { powered_off_ = false; }
  bool powered_off() const { return powered_off_; }

  // The next n appends succeed, then every append fails. Negative disables.
  void FailWritesAfter(int64_t n) { writes_left_ = n; }
  void FailSyncs(bool fail) { fail_syncs_ = fail; }
  void ClearFaults() {
    writes_left_ = -1;
    fail_syncs_ = false;
  }

  Status OnAppend() override;
  Status OnSync() override;
  Status OnMetadataChange() override;
  Status OnSyncDir(const std::string& dir) override;

 private:
  std::atomic<bool> powered_off_{false};
  std::atomic<int64_t> writes_left_{-1};
  std::atomic<bool> fail_syncs_{false};
  std::map<std::string, std::shared_ptr<FileState>> durable_files_;  // guarded by mu_
};

}  // namespace docstore
