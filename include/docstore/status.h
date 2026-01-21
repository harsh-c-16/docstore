#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace docstore {

// Result of every fallible operation. The public API never throws; errors travel as Status.
// [[nodiscard]] makes the compiler flag any call site that silently drops an error.
class [[nodiscard]] Status {
 public:
  enum class Code : uint8_t {
    kOk = 0,
    kNotFound,
    kDuplicateKey,
    kInvalidArgument,
    kCorruption,
    kIOError,
    kReadOnly,  // the database hit a background error and refuses writes
  };

  Status() = default;

  static Status OK() { return Status(); }
  static Status NotFound(std::string_view msg = {}) { return Status(Code::kNotFound, msg); }
  static Status DuplicateKey(std::string_view msg = {}) { return Status(Code::kDuplicateKey, msg); }
  static Status InvalidArgument(std::string_view msg = {}) {
    return Status(Code::kInvalidArgument, msg);
  }
  static Status Corruption(std::string_view msg = {}) { return Status(Code::kCorruption, msg); }
  static Status IOError(std::string_view msg = {}) { return Status(Code::kIOError, msg); }
  static Status ReadOnly(std::string_view msg = {}) { return Status(Code::kReadOnly, msg); }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsDuplicateKey() const { return code_ == Code::kDuplicateKey; }
  bool IsInvalidArgument() const { return code_ == Code::kInvalidArgument; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  bool IsReadOnly() const { return code_ == Code::kReadOnly; }

  Code code() const { return code_; }
  const std::string& message() const { return msg_; }

  // "NotFound", "DuplicateKey", ... (also used as the wire error code by the server).
  std::string_view CodeName() const;
  // "OK" or "<CodeName>: <message>".
  std::string ToString() const;

 private:
  Status(Code code, std::string_view msg) : code_(code), msg_(msg) {}

  Code code_ = Code::kOk;
  std::string msg_;
};

}  // namespace docstore
