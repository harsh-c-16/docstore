#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>

#include "env/mem_env.h"
#include "server/command_handler.h"
#include "server/tcp_server.h"

namespace docstore {
namespace {

using json = nlohmann::json;

// A tiny blocking line client.
class Client {
 public:
  explicit Client(int port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  }
  ~Client() { ::close(fd_); }
  bool connected() const { return connected_; }

  bool SendRaw(const std::string& data) {
    return ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(data.size());
  }
  // Returns the next line, or "" if the server closed the connection.
  std::string ReadLine() {
    while (true) {
      size_t nl = buf_.find('\n');
      if (nl != std::string::npos) {
        std::string line = buf_.substr(0, nl);
        buf_.erase(0, nl + 1);
        return line;
      }
      char chunk[65536];
      ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
      if (n <= 0) return "";
      buf_.append(chunk, static_cast<size_t>(n));
    }
  }
  json Call(const json& req) {
    if (!SendRaw(req.dump() + "\n")) return json();
    std::string line = ReadLine();
    return line.empty() ? json() : json::parse(line);
  }

 private:
  int fd_;
  bool connected_ = false;
  std::string buf_;
};

class ServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    options_.env = &env_;
    ASSERT_TRUE(Database::Open(options_, "/db", &db_).ok());
    handler_ = std::make_unique<CommandHandler>(db_.get());
  }
  void StartServer(int max_conns) {
    server_ = std::make_unique<TcpServer>(handler_.get(), max_conns);
    ASSERT_TRUE(server_->Start("127.0.0.1", 0).ok());
  }

  MemEnv env_;
  Options options_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<CommandHandler> handler_;
  std::unique_ptr<TcpServer> server_;
};

TEST_F(ServerTest, SixteenConcurrentClientsTimesAThousandOps) {
  StartServer(256);
  ASSERT_TRUE(handler_->Handle({{"op", "createIndex"}, {"coll", "load"}, {"field", "client"}})["ok"] == 1);
  constexpr int kClients = 16, kOps = 1000;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int c = 0; c < kClients; c++) {
    threads.emplace_back([&, c] {
      Client client(server_->port());
      if (!client.connected()) {
        failures++;
        return;
      }
      // Each op is checked against what this client wrote: insert, read back, update, query.
      for (int i = 0; i < kOps; i++) {
        const std::string id = "c" + std::to_string(c) + "-" + std::to_string(i / 4);
        json r;
        switch (i % 4) {
          case 0:
            r = client.Call({{"op", "insert"}, {"coll", "load"},
                             {"doc", {{"_id", id}, {"client", c}, {"v", 0}}}});
            if (r["ok"] != 1 || r["insertedId"] != id) failures++;
            break;
          case 1:
            r = client.Call({{"op", "update"}, {"coll", "load"}, {"id", id},
                             {"update", {{"$inc", {{"v", 5}}}}}});
            if (r["ok"] != 1) failures++;
            break;
          case 2:
            r = client.Call({{"op", "get"}, {"coll", "load"}, {"id", id}});
            if (r["ok"] != 1 || r["doc"]["v"] != 5 || r["doc"]["client"] != c) failures++;
            break;
          default:
            r = client.Call({{"op", "find"}, {"coll", "load"}, {"filter", {{"client", c}}}});
            if (r["ok"] != 1 || r["docs"].size() != static_cast<size_t>(i / 4 + 1)) failures++;
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(failures.load(), 0);
  json all = handler_->Handle({{"op", "find"}, {"coll", "load"}, {"filter", json::object()}});
  EXPECT_EQ(all["docs"].size(), static_cast<size_t>(kClients * kOps / 4));
}

TEST_F(ServerTest, ConnectionLimitBadInputAndOversizedLines) {
  StartServer(2);
  Client a(server_->port()), b(server_->port());
  ASSERT_EQ(a.Call({{"op", "ping"}})["ok"], 1);
  ASSERT_EQ(b.Call({{"op", "ping"}})["ok"], 1);
  Client c(server_->port());  // third connection: over the limit
  std::string rejected = c.ReadLine();
  EXPECT_NE(rejected.find("too many connections"), std::string::npos) << rejected;
  EXPECT_EQ(c.ReadLine(), "");  // and closed

  ASSERT_TRUE(a.SendRaw("this is not json\n\n{\"op\":\"ping\"}\r\n"));
  EXPECT_EQ(json::parse(a.ReadLine())["code"], "InvalidArgument");
  EXPECT_EQ(json::parse(a.ReadLine())["ok"], 1);  // blank line skipped, CRLF accepted

  std::string huge(TcpServer::kMaxLineBytes + 1024, 'x');  // no newline at all
  b.SendRaw(huge);
  std::string err = b.ReadLine();
  EXPECT_NE(err.find("longer than 17 MiB"), std::string::npos);
  EXPECT_EQ(b.ReadLine(), "");  // closed after the error
  EXPECT_EQ(a.Call({{"op", "ping"}})["ok"], 1);  // other clients unaffected
}

TEST_F(ServerTest, StopIsGracefulWithOpenConnections) {
  StartServer(16);
  Client a(server_->port());
  ASSERT_EQ(a.Call({{"op", "insert"}, {"coll", "x"}, {"doc", {{"_id", "1"}}}})["ok"], 1);
  server_->Stop();  // returns only after the connection thread has exited
  EXPECT_EQ(a.ReadLine(), "");
  Client late(server_->port());
  EXPECT_FALSE(late.connected());
  json doc;
  EXPECT_TRUE(db_->Get("x", "1", &doc).ok());  // the database is intact and still usable
}

}  // namespace
}  // namespace docstore
