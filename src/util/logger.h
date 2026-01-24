#pragma once

// Minimal leveled logging to stderr. The threshold comes from DOCSTORE_LOG_LEVEL
// (debug|info|warn|error|off, default warn) so tests stay quiet unless something is wrong.

#include <string_view>

namespace docstore {

enum class LogLevel { kDebug = 0, kInfo, kWarn, kError, kOff };

void Log(LogLevel level, std::string_view message);

}  // namespace docstore
