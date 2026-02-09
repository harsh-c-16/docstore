#include "kv/filename.h"

#include <cstdio>

namespace docstore::kv {
namespace {

std::string Numbered(const std::string& dbname, uint64_t number, const char* suffix) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s", static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

bool ParseNumber(const std::string& digits, uint64_t* number) {
  if (digits.empty() || digits.size() > 19) return false;
  uint64_t v = 0;
  for (char c : digits) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  *number = v;
  return true;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::string LogFileName(const std::string& dbname, uint64_t number) {
  return Numbered(dbname, number, "log");
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  return Numbered(dbname, number, "sst");
}

std::string ManifestFileName(const std::string& dbname, uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu", static_cast<unsigned long long>(number));
  return dbname + buf;
}

std::string CurrentFileName(const std::string& dbname) { return dbname + "/CURRENT"; }

std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }

FileType ParseFileName(const std::string& name, uint64_t* number) {
  if (name == "CURRENT") return FileType::kCurrent;
  if (name == "LOCK") return FileType::kLock;
  if (EndsWith(name, ".tmp")) return FileType::kTemp;
  if (name.rfind("MANIFEST-", 0) == 0) {
    return ParseNumber(name.substr(9), number) ? FileType::kManifest : FileType::kUnknown;
  }
  size_t dot = name.find('.');
  if (dot == std::string::npos || !ParseNumber(name.substr(0, dot), number)) {
    return FileType::kUnknown;
  }
  std::string ext = name.substr(dot + 1);
  if (ext == "log") return FileType::kLog;
  if (ext == "sst") return FileType::kTable;
  return FileType::kUnknown;
}

}  // namespace docstore::kv
