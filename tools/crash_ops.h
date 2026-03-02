#pragma once

// The deterministic workload shared by crash_writer and the process-crash test, so the test can
// compute exactly what the database must contain after the first n acknowledged ops.

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

namespace docstore::crash {

struct CrashOp {
  std::string key;
  std::string value;  // empty = delete
  bool sync;
};

inline CrashOp OpAt(uint64_t i) {
  char key[16];
  std::snprintf(key, sizeof(key), "slot%04llu", static_cast<unsigned long long>(i * 7919 % 1000));
  CrashOp op;
  op.key = key;
  if (i % 10 != 9) op.value = "v" + std::to_string(i) + std::string(50, 'x');
  op.sync = i % 50 == 0;  // a mix of sync=true and sync=false writes
  return op;
}

inline std::map<std::string, std::string> StateAfter(uint64_t n) {
  std::map<std::string, std::string> state;
  for (uint64_t i = 0; i < n; i++) {
    CrashOp op = OpAt(i);
    if (op.value.empty()) {
      state.erase(op.key);
    } else {
      state[op.key] = op.value;
    }
  }
  return state;
}

}  // namespace docstore::crash
