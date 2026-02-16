#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "env/env.h"
#include "env/mem_env.h"
#include "kv/log_reader.h"
#include "kv/log_writer.h"
#include "kv/memtable.h"
#include "kv/write_batch.h"
#include "util/coding.h"
#include "util/random.h"

namespace docstore::kv {
namespace {

class RecordingHandler : public WriteBatch::Handler {
 public:
  void Put(std::string_view k, std::string_view v) override {
    ops.push_back("put " + std::string(k) + "=" + std::string(v));
  }
  void Delete(std::string_view k) override { ops.push_back("del " + std::string(k)); }
  std::vector<std::string> ops;
};

TEST(WriteBatchTest, EncodeIterateAndSequence) {
  WriteBatch b;
  EXPECT_EQ(b.Count(), 0u);
  b.Put("a", "1");
  b.Delete("b");
  b.Put("", "");
  b.SetSequence(100);
  EXPECT_EQ(b.Count(), 3u);
  EXPECT_EQ(b.Sequence(), 100u);

  WriteBatch copy;
  ASSERT_TRUE(copy.SetContents(b.Contents()).ok());
  RecordingHandler h;
  ASSERT_TRUE(copy.Iterate(&h).ok());
  EXPECT_EQ(h.ops, (std::vector<std::string>{"put a=1", "del b", "put ="}));

  MemTable mem;
  ASSERT_TRUE(InsertInto(copy, &mem).ok());
  std::string v;
  EXPECT_EQ(mem.Get("a", 100, &v), MemTable::LookupResult::kFound);
  EXPECT_EQ(mem.Get("a", 99, &v), MemTable::LookupResult::kNotPresent);  // seq 100 = first
  EXPECT_EQ(mem.Get("b", 101, &v), MemTable::LookupResult::kDeleted);
  EXPECT_EQ(mem.Get("", 102, &v), MemTable::LookupResult::kFound);
}

TEST(WriteBatchTest, MalformedContentsAreCorruption) {
  WriteBatch b;
  EXPECT_TRUE(b.SetContents("short").IsCorruption());

  WriteBatch good;
  good.Put("key", "value");
  std::string rep(good.Contents());

  WriteBatch truncated;
  ASSERT_TRUE(truncated.SetContents(rep.substr(0, rep.size() - 2)).ok());
  RecordingHandler h;
  EXPECT_TRUE(truncated.Iterate(&h).IsCorruption());

  std::string wrong_count = rep;
  EncodeFixed32(wrong_count.data() + 8, 2);
  WriteBatch c;
  ASSERT_TRUE(c.SetContents(wrong_count).ok());
  EXPECT_TRUE(c.Iterate(&h).IsCorruption());

  std::string bad_type = rep;
  bad_type[WriteBatch::kHeaderSize] = 9;
  ASSERT_TRUE(c.SetContents(bad_type).ok());
  EXPECT_TRUE(c.Iterate(&h).IsCorruption());
}

class WalTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(env_.CreateDir("/db").ok()); }

  // Writes the records and returns the raw file contents.
  std::string WriteLog(const std::vector<std::string>& records) {
    std::unique_ptr<WritableFile> file;
    EXPECT_TRUE(env_.NewWritableFile("/db/000001.log", &file).ok());
    LogWriter writer(std::move(file));
    for (const auto& r : records) EXPECT_TRUE(writer.AddRecord(r).ok());
    EXPECT_TRUE(writer.Sync().ok());
    std::string contents;
    EXPECT_TRUE(ReadFileToString(&env_, "/db/000001.log", &contents).ok());
    return contents;
  }

  static std::vector<std::string> ReadAll(LogReader* reader) {
    std::vector<std::string> out;
    std::string_view rec;
    while (reader->ReadRecord(&rec)) out.emplace_back(rec);
    return out;
  }

  static std::vector<std::string> Records(int n) {
    std::vector<std::string> out;
    Random rnd(n);
    for (int i = 0; i < n; i++) out.push_back(std::string(1 + rnd.Uniform(300), 'a' + i % 26));
    return out;
  }

  MemEnv env_;
};

TEST_F(WalTest, RoundTripIncludingEmptyAndLargeRecords) {
  std::vector<std::string> records = {"", "small", std::string(1 << 20, 'L'), "after-large"};
  std::string contents = WriteLog(records);
  LogReader reader(contents);
  EXPECT_EQ(ReadAll(&reader), records);
  EXPECT_TRUE(reader.status().ok());
  EXPECT_EQ(reader.valid_prefix_length(), contents.size());
}

TEST_F(WalTest, TornTailIsDetectedAndCutAtLastGoodRecord) {
  std::vector<std::string> records = Records(10);
  std::string contents = WriteLog(records);
  size_t last_start = contents.size() - kLogHeaderSize - records.back().size();
  // Cut inside the payload, and also inside the header, of the final record.
  for (size_t cut : {contents.size() - 1, last_start + 3, last_start + kLogHeaderSize}) {
    std::string torn = contents.substr(0, cut);
    LogReader reader(torn);
    auto got = ReadAll(&reader);
    EXPECT_EQ(got, std::vector<std::string>(records.begin(), records.end() - 1)) << cut;
    EXPECT_EQ(reader.problem(), LogReader::Problem::kIncomplete);
    EXPECT_TRUE(reader.status().IsCorruption());
    EXPECT_EQ(reader.valid_prefix_length(), last_start);
    EXPECT_TRUE(reader.BadRecordIsTornTail()) << cut;
  }
}

TEST_F(WalTest, MidFileCorruptionIsDetectedAndNeverReturnsLaterRecords) {
  std::vector<std::string> records = Records(10);
  std::string contents = WriteLog(records);
  size_t third = 0;
  for (int i = 0; i < 2; i++) third += kLogHeaderSize + records[i].size();
  std::string bad = contents;
  bad[third + kLogHeaderSize + 1] ^= 0x40;  // flip a payload bit of record #3
  LogReader reader(bad);
  auto got = ReadAll(&reader);
  EXPECT_EQ(got, std::vector<std::string>(records.begin(), records.begin() + 2));
  EXPECT_EQ(reader.problem(), LogReader::Problem::kChecksum);
  EXPECT_FALSE(reader.BadRecordIsTornTail());
}

TEST_F(WalTest, OversizedLengthIsCorruptionNotAllocation) {
  std::vector<std::string> records = {"ok", std::string(10, 'v')};
  std::string contents = WriteLog(records);
  size_t second = kLogHeaderSize + 2;
  EncodeFixed32(contents.data() + second + 4, 0xFFFFFFF0u);
  LogReader reader(contents);
  EXPECT_EQ(ReadAll(&reader), std::vector<std::string>{"ok"});
  EXPECT_EQ(reader.problem(), LogReader::Problem::kOversized);
  EXPECT_FALSE(reader.BadRecordIsTornTail());
}

TEST_F(WalTest, DamagedLengthIsNotMistakenForTornTail) {
  std::vector<std::string> records = Records(6);
  std::string contents = WriteLog(records);
  // Last record: a larger length makes it look cut off, but its CRC matches at the true length.
  std::string last = contents;
  size_t last_start = contents.size() - kLogHeaderSize - records.back().size();
  EncodeFixed32(last.data() + last_start + 4, static_cast<uint32_t>(records.back().size() + 50));
  LogReader r1(last);
  ReadAll(&r1);
  EXPECT_EQ(r1.problem(), LogReader::Problem::kIncomplete);
  EXPECT_FALSE(r1.BadRecordIsTornTail());

  // Middle record: a huge-but-legal length runs past end of file; later records still verify.
  std::string mid = contents;
  EncodeFixed32(mid.data() + 4, static_cast<uint32_t>(contents.size()));
  LogReader r2(mid);
  EXPECT_TRUE(ReadAll(&r2).empty());
  EXPECT_EQ(r2.problem(), LogReader::Problem::kIncomplete);
  EXPECT_FALSE(r2.BadRecordIsTornTail());
}

}  // namespace
}  // namespace docstore::kv
