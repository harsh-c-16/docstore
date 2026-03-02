// Process-crash test: fork/exec crash_writer, SIGKILL it after a random 50-2000 ms, reopen the
// database and check that every op it printed (= acknowledged) is present and that the state is
// exactly a prefix of the op sequence.

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "crash_ops.h"
#include "kv/engine.h"
#include "test_util.h"
#include "util/random.h"

namespace docstore::kv {
namespace {

// Runs crash_writer on db for about kill_after_ms, kills it, and returns everything it printed.
std::string RunAndKill(const std::string& db, int kill_after_ms) {
  int fds[2];
  EXPECT_EQ(::pipe(fds), 0);
  pid_t pid = ::fork();
  if (pid == 0) {
    ::dup2(fds[1], STDOUT_FILENO);
    ::close(fds[0]);
    ::close(fds[1]);
    ::execl(CRASH_WRITER_PATH, "crash_writer", db.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  ::close(fds[1]);
  std::string out;
  char buf[65536];
  // Keep draining the pipe while waiting, or the child would block on a full pipe.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kill_after_ms);
  bool eof = false;
  while (!eof) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now())
                    .count();
    if (left <= 0) break;
    pollfd p{fds[0], POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(left)) > 0) {
      ssize_t n = ::read(fds[0], buf, sizeof(buf));
      if (n <= 0) eof = true;
      if (n > 0) out.append(buf, static_cast<size_t>(n));
    }
  }
  ::kill(pid, SIGKILL);
  ssize_t n;
  while ((n = ::read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
  ::close(fds[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
      << "crash_writer exited on its own with status " << status;
  return out;
}

TEST(ProcessCrashTest, AcknowledgedWritesSurviveSigkill) {
  const int iterations = test::LongMode() ? 200 : 10;
  Random rnd(test::TestSeed());
  uint64_t total_acked = 0;
  for (int iter = 0; iter < iterations; iter++) {
    const std::string db = test::NewTempDir("crash");
    const int kill_after_ms = 50 + static_cast<int>(rnd.Uniform(1951));
    SCOPED_TRACE("iteration " + std::to_string(iter) + ", killed after " +
                 std::to_string(kill_after_ms) + " ms, db " + db);
    std::string out = RunAndKill(db, kill_after_ms);

    // Acknowledged ops are the complete lines; they must be 0, 1, 2, ... in order.
    uint64_t acked = 0;
    size_t pos = 0;
    for (size_t nl; (nl = out.find('\n', pos)) != std::string::npos; pos = nl + 1) {
      ASSERT_EQ(std::stoull(out.substr(pos, nl - pos)), acked);
      acked++;
    }
    total_acked += acked;

    std::unique_ptr<Engine> engine;
    Status s = Engine::Open(Options(), db, &engine);
    ASSERT_TRUE(s.ok()) << s.ToString();
    std::vector<std::pair<std::string, std::string>> rows;
    ASSERT_TRUE(engine->Scan(ReadOptions(), 0, &rows).ok());
    std::map<std::string, std::string> recovered(rows.begin(), rows.end());
    // Exactly the acknowledged prefix, or that plus the one op in flight when it was killed.
    bool ok = recovered == crash::StateAfter(acked) || recovered == crash::StateAfter(acked + 1);
    ASSERT_TRUE(ok) << "state is not the acknowledged prefix of " << acked << " ops";
    engine.reset();
    std::filesystem::remove_all(db);
  }
  RecordProperty("iterations", iterations);
  RecordProperty("acknowledged_ops", static_cast<int>(total_acked));
  std::printf("process-crash: %d SIGKILL runs, %llu acknowledged ops, 0 lost\n", iterations,
              static_cast<unsigned long long>(total_acked));
}

}  // namespace
}  // namespace docstore::kv
