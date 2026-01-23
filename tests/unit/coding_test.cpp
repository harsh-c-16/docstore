#include "util/coding.h"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace docstore {
namespace {

TEST(CodingTest, FixedRoundTrip) {
  std::string buf;
  PutFixed32(&buf, 0xDEADBEEF);
  PutFixed64(&buf, 0x0123456789ABCDEFull);
  ASSERT_EQ(buf.size(), 12u);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0xEF);  // little-endian
  std::string_view in = buf;
  uint32_t a;
  uint64_t b;
  ASSERT_TRUE(GetFixed32(&in, &a));
  ASSERT_TRUE(GetFixed64(&in, &b));
  EXPECT_EQ(a, 0xDEADBEEFu);
  EXPECT_EQ(b, 0x0123456789ABCDEFull);
  EXPECT_TRUE(in.empty());
  EXPECT_FALSE(GetFixed32(&in, &a));
}

TEST(CodingTest, VarintRoundTripAndLength) {
  std::vector<uint64_t> values = {0, 1, 127, 128, 255, 300, 16383, 16384, UINT32_MAX,
                                  1ull << 35, std::numeric_limits<uint64_t>::max()};
  std::string buf;
  for (uint64_t v : values) PutVarint64(&buf, v);
  std::string_view in = buf;
  for (uint64_t v : values) {
    uint64_t got;
    size_t before = in.size();
    ASSERT_TRUE(GetVarint64(&in, &got));
    EXPECT_EQ(got, v);
    EXPECT_EQ(static_cast<int>(before - in.size()), VarintLength(v));
  }
  EXPECT_TRUE(in.empty());
}

TEST(CodingTest, Varint32RejectsOverflowAndTruncation) {
  std::string buf;
  PutVarint64(&buf, 1ull << 33);
  std::string_view in = buf;
  uint32_t v;
  EXPECT_FALSE(GetVarint32(&in, &v));

  std::string truncated;
  PutVarint32(&truncated, 1u << 30);
  truncated.pop_back();
  in = truncated;
  EXPECT_FALSE(GetVarint32(&in, &v));

  std::string too_long(11, '\x80');
  in = too_long;
  uint64_t w;
  EXPECT_FALSE(GetVarint64(&in, &w));
}

TEST(CodingTest, LengthPrefixed) {
  std::string buf;
  PutLengthPrefixed(&buf, "hello");
  PutLengthPrefixed(&buf, std::string_view("\0x", 2));
  std::string_view in = buf, a, b;
  ASSERT_TRUE(GetLengthPrefixed(&in, &a));
  ASSERT_TRUE(GetLengthPrefixed(&in, &b));
  EXPECT_EQ(a, "hello");
  EXPECT_EQ(b, std::string_view("\0x", 2));
  std::string bad;
  PutVarint32(&bad, 10);
  bad += "abc";
  in = bad;
  EXPECT_FALSE(GetLengthPrefixed(&in, &a));
}

TEST(CodingTest, BigEndianOrderMatchesNumericOrder) {
  std::string a, b;
  PutBigEndian32(&a, 255);
  PutBigEndian32(&b, 256);
  EXPECT_LT(a, b);  // bytewise compare
  EXPECT_EQ(DecodeBigEndian32(b.data()), 256u);
  std::string c;
  PutBigEndian64(&c, 0x0102030405060708ull);
  EXPECT_EQ(c[0], 1);
  EXPECT_EQ(DecodeBigEndian64(c.data()), 0x0102030405060708ull);
}

}  // namespace
}  // namespace docstore
