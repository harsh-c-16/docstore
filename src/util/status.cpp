#include "docstore/status.h"

namespace docstore {

std::string_view Status::CodeName() const {
  switch (code_) {
    case Code::kOk:
      return "OK";
    case Code::kNotFound:
      return "NotFound";
    case Code::kDuplicateKey:
      return "DuplicateKey";
    case Code::kInvalidArgument:
      return "InvalidArgument";
    case Code::kCorruption:
      return "Corruption";
    case Code::kIOError:
      return "IOError";
    case Code::kReadOnly:
      return "ReadOnly";
  }
  return "Unknown";
}

std::string Status::ToString() const {
  if (ok()) return "OK";
  std::string out(CodeName());
  if (!msg_.empty()) {
    out += ": ";
    out += msg_;
  }
  return out;
}

}  // namespace docstore
