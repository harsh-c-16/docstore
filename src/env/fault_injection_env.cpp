#include "env/fault_injection_env.h"

namespace docstore {

void FaultInjectionEnv::SimulatePowerLoss() {
  std::lock_guard<std::mutex> l(mu_);
  // Set the flag first: an Append that already holds a file mutex either sees the flag, or
  // finishes before we truncate that file below (we take the same mutex), so nothing written
  // after this point can survive.
  powered_off_ = true;
  files_ = durable_files_;
  for (auto& [path, state] : files_) {
    std::lock_guard<std::mutex> fl(state->mu);
    state->data.resize(state->synced_size);
  }
}

Status FaultInjectionEnv::OnAppend() {
  if (powered_off_) return Status::IOError("simulated power loss");
  int64_t left = writes_left_.load();
  while (left >= 0) {
    if (left == 0) return Status::IOError("injected write failure");
    if (writes_left_.compare_exchange_weak(left, left - 1)) break;
  }
  return Status::OK();
}

Status FaultInjectionEnv::OnSync() {
  if (powered_off_) return Status::IOError("simulated power loss");
  if (fail_syncs_) return Status::IOError("injected sync failure");
  return Status::OK();
}

Status FaultInjectionEnv::OnMetadataChange() {
  if (powered_off_) return Status::IOError("simulated power loss");
  return Status::OK();
}

Status FaultInjectionEnv::OnSyncDir(const std::string& dir) {
  Status s = OnSync();
  if (!s.ok()) return s;
  // The durable namespace of this directory becomes exactly the visible one.
  for (auto it = durable_files_.begin(); it != durable_files_.end();) {
    it = Parent(it->first) == dir ? durable_files_.erase(it) : std::next(it);
  }
  for (const auto& [path, state] : files_) {
    if (Parent(path) == dir) durable_files_[path] = state;
  }
  return Status::OK();
}

}  // namespace docstore
