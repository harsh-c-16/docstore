#pragma once

// Names of the files inside a database directory. All numbered files share one counter.
//   000005.log         write-ahead log
//   000007.sst         sorted table
//   MANIFEST-000006    version edit log
//   CURRENT            name of the live MANIFEST
//   LOCK               held while the database is open

#include <cstdint>
#include <string>

namespace docstore::kv {

enum class FileType { kLog, kTable, kManifest, kCurrent, kLock, kTemp, kUnknown };

std::string LogFileName(const std::string& dbname, uint64_t number);
std::string TableFileName(const std::string& dbname, uint64_t number);
std::string ManifestFileName(const std::string& dbname, uint64_t number);
std::string CurrentFileName(const std::string& dbname);
std::string LockFileName(const std::string& dbname);

// Parses a bare file name (no directory). *number is set for numbered files.
FileType ParseFileName(const std::string& name, uint64_t* number);

}  // namespace docstore::kv
