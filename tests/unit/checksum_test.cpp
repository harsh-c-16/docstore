#include <gtest/gtest.h>

#include <string>

#include "util/crc32c.h"
#include "util/hash.h"

namespace docstore {
namespace {

// Reference values were produced with the Python `crc32c` and `xxhash` packages.
TEST(Crc32cTest, KnownVectors) {
  EXPECT_EQ(crc32c::Value("123456789"), 0xE3069283u);
  EXPECT_EQ(crc32c::Value(std::string(32, '\0')), 0x8A9136AAu);
  EXPECT_EQ(crc32c::Value(std::string(32, '\xff')), 0x62A8AB43u);
  std::string ascending;
  for (int i = 0; i < 32; i++) ascending.push_back(static_cast<char>(i));
  EXPECT_EQ(crc32c::Value(ascending), 0x46DD794Eu);
}

TEST(Crc32cTest, ExtendEqualsWhole) {
  std::string s = "hello crc32c world";
  uint32_t part = crc32c::Value(s.data(), 5);
  EXPECT_EQ(crc32c::Extend(part, s.data() + 5, s.size() - 5), crc32c::Value(s));
  EXPECT_NE(crc32c::Value("a"), crc32c::Value("b"));
}

TEST(Hash64Test, KnownVectors) {
  EXPECT_EQ(Hash64(""), 0xEF46DB3751D8E999ull);
  EXPECT_EQ(Hash64("a"), 0xD24EC4F1A98C6E5Bull);
  EXPECT_EQ(Hash64("abc"), 0x44BC2CF5AD770999ull);
  EXPECT_EQ(Hash64("Nobody inspects the spammish repetition"), 0xFBCEA83C8A378BF1ull);
  std::string hundred;
  for (int i = 0; i < 100; i++) hundred.push_back(static_cast<char>(i));
  EXPECT_EQ(Hash64(hundred), 0x6AC1E58032166597ull);  // exercises the 32-byte stripe loop
  EXPECT_EQ(Hash64(hundred, 42), 0x819D2B726001D507ull);
  EXPECT_EQ(Hash64("abc", 42), 0x13C1D910702770E6ull);
}

}  // namespace
}  // namespace docstore
