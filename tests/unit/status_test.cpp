#include "docstore/status.h"

#include <gtest/gtest.h>

namespace docstore {
namespace {

TEST(StatusTest, OkByDefault) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(StatusTest, CodesAndMessages) {
  Status s = Status::NotFound("users/42");
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsNotFound());
  EXPECT_EQ(s.CodeName(), "NotFound");
  EXPECT_EQ(s.ToString(), "NotFound: users/42");
  EXPECT_EQ(Status::Corruption().ToString(), "Corruption");
  EXPECT_TRUE(Status::DuplicateKey("x").IsDuplicateKey());
  EXPECT_TRUE(Status::ReadOnly("bg").IsReadOnly());
  EXPECT_EQ(Status::InvalidArgument("bad").CodeName(), "InvalidArgument");
}

}  // namespace
}  // namespace docstore
