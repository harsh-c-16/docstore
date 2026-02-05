#pragma once

// Record log used for the WAL (NNNNNN.log) and the MANIFEST.
// Record: crc32c u32 | length u32 | payload, where the CRC covers the length and the payload.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

#include "env/env.h"

namespace docstore::kv {

constexpr size_t kLogHeaderSize = 8;
// Larger lengths are treated as corruption so a garbage length never causes a huge allocation.
constexpr uint32_t kMaxLogRecordSize = 64u << 20;

class LogWriter {
 public:
  explicit LogWriter(std::unique_ptr<WritableFile> file) : file_(std::move(file)) {}
  LogWriter(const LogWriter&) = delete;
  LogWriter& operator=(const LogWriter&) = delete;

  // Appends one record and hands it to the OS, so it survives a process crash on return.
  // Thread-safe.
  Status AddRecord(std::string_view payload);

  // Makes every record added so far durable. The buffer flush happens under the mutex, the
  // fdatasync outside it, so a slow sync never blocks writers (DESIGN.md D6).
  Status Sync();

  uint64_t bytes_written() const;

 private:
  mutable std::mutex mu_;
  std::unique_ptr<WritableFile> file_;
  uint64_t bytes_written_ = 0;  // guarded by mu_
};

}  // namespace docstore::kv
