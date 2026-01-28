#pragma once

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

namespace docstore::test {

// A fresh, empty directory under TEST_TMPDIR (set by CMake to build/<preset>/test_tmp).
inline std::string NewTempDir(const std::string& name) {
  static std::atomic<int> counter{0};
  std::string dir = ::testing::TempDir() + "docstore_" + name + "_" + std::to_string(::getpid()) +
                    "_" + std::to_string(counter++);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

// True when the long suite is running (ctest -L long sets DOCSTORE_LONG=1).
inline bool LongMode() {
  const char* v = std::getenv("DOCSTORE_LONG");
  return v != nullptr && std::string(v) == "1";
}

// Seed from DOCSTORE_TEST_SEED, or a random one. Tests print it on failure.
inline uint64_t TestSeed() {
  if (const char* v = std::getenv("DOCSTORE_TEST_SEED")) return std::strtoull(v, nullptr, 10);
  return std::random_device{}();
}

}  // namespace docstore::test
