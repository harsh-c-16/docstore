#include "doc/objectid.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>

namespace docstore::doc {
namespace {

struct ProcessState {
  std::array<uint8_t, 5> random{};
  std::atomic<uint32_t> counter{0};
  ProcessState() {
    std::random_device rd;
    for (auto& b : random) b = static_cast<uint8_t>(rd());
    // Start below 2^23 so the 24-bit counter cannot wrap within a test-sized burst, which would
    // break the ordering of ids created in the same second.
    counter = rd() & 0x7FFFFF;
  }
};

ProcessState& State() {
  static ProcessState state;
  return state;
}

}  // namespace

std::string GenerateObjectId() {
  static std::mutex mu;
  ProcessState& st = State();
  uint8_t bytes[12];
  {
    // The lock keeps (seconds, counter) pairs in order across threads.
    std::lock_guard<std::mutex> l(mu);
    const auto secs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count());
    const uint32_t count = st.counter.fetch_add(1) & 0xFFFFFF;
    for (int i = 0; i < 4; i++) bytes[i] = static_cast<uint8_t>(secs >> (24 - 8 * i));
    for (int i = 0; i < 5; i++) bytes[4 + i] = st.random[i];
    for (int i = 0; i < 3; i++) bytes[9 + i] = static_cast<uint8_t>(count >> (16 - 8 * i));
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(24, '0');
  for (int i = 0; i < 12; i++) {
    out[2 * i] = kHex[bytes[i] >> 4];
    out[2 * i + 1] = kHex[bytes[i] & 0xF];
  }
  return out;
}

}  // namespace docstore::doc
