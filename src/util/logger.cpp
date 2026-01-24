#include "util/logger.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace docstore {
namespace {

LogLevel ThresholdFromEnv() {
  const char* env = std::getenv("DOCSTORE_LOG_LEVEL");
  std::string_view v = env ? env : "warn";
  if (v == "debug") return LogLevel::kDebug;
  if (v == "info") return LogLevel::kInfo;
  if (v == "error") return LogLevel::kError;
  if (v == "off") return LogLevel::kOff;
  return LogLevel::kWarn;
}

}  // namespace

void Log(LogLevel level, std::string_view message) {
  static const LogLevel threshold = ThresholdFromEnv();  // thread-safe static init
  if (level < threshold) return;
  static constexpr const char* kNames[] = {"DEBUG", "INFO", "WARN", "ERROR"};
  // One fprintf call per line so concurrent log lines do not interleave mid-line.
  std::fprintf(stderr, "[docstore %s] %.*s\n", kNames[static_cast<int>(level)],
               static_cast<int>(message.size()), message.data());
}

}  // namespace docstore
