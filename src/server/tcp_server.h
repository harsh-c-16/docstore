#pragma once

// Newline-delimited JSON over TCP: one request per line, one response per line.
// One thread per connection, capped at max_conns (extra connections get an error line and are
// closed). Lines longer than kMaxLineBytes get an error response and the connection is closed.
// Stop() is graceful: stop accepting, let in-flight requests finish, wait for every connection.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "docstore/status.h"
#include "server/command_handler.h"

namespace docstore {

class TcpServer {
 public:
  static constexpr size_t kMaxLineBytes = 17 << 20;

  TcpServer(CommandHandler* handler, int max_conns) : handler_(handler), max_conns_(max_conns) {}
  ~TcpServer() { Stop(); }
  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  // Binds host:port (port 0 picks a free port) and starts accepting in a background thread.
  Status Start(const std::string& host, int port);
  int port() const { return port_; }
  // Idempotent. Returns once every connection thread has exited.
  void Stop();

 private:
  void AcceptLoop();
  void Serve(int fd);

  CommandHandler* handler_;
  const int max_conns_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::thread accept_thread_;
  std::atomic<bool> stopping_{false};

  std::mutex mu_;
  std::condition_variable all_done_;
  std::set<int> conns_;  // open connection sockets, guarded by mu_
  int active_ = 0;       // running connection threads, guarded by mu_
};

}  // namespace docstore
