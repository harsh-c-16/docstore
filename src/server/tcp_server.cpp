#include "server/tcp_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace docstore {
namespace {

bool SendAll(int fd, std::string_view data) {
  while (!data.empty()) {
    // MSG_NOSIGNAL: a client that hung up must not kill the server with SIGPIPE.
    ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    data.remove_prefix(static_cast<size_t>(n));
  }
  return true;
}

std::string ErrorLine(const Status& s) { return ErrorResponse(s).dump() + "\n"; }

}  // namespace

Status TcpServer::Start(const std::string& host, int port) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) return Status::IOError(std::string("socket: ") + std::strerror(errno));
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    return Status::InvalidArgument("bad IPv4 address: " + host);
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(listen_fd_, 128) != 0) {
    return Status::IOError("bind/listen on " + host + ":" + std::to_string(port) + ": " +
                           std::strerror(errno));
  }
  socklen_t len = sizeof(addr);
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  accept_thread_ = std::thread([this] { AcceptLoop(); });
  return Status::OK();
}

void TcpServer::AcceptLoop() {
  while (!stopping_) {
    int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      break;  // Stop() shut the listening socket down
    }
    std::lock_guard<std::mutex> l(mu_);
    if (stopping_ || active_ >= max_conns_) {
      SendAll(fd, ErrorLine(Status::IOError("too many connections")));
      ::close(fd);
      continue;
    }
    active_++;
    conns_.insert(fd);
    // Detached: Stop() waits on active_ instead of joining, so finished connections never
    // pile up as unjoined threads.
    std::thread([this, fd] { Serve(fd); }).detach();
  }
}

void TcpServer::Serve(int fd) {
  std::string buffer;
  size_t scanned = 0;  // buffer[0, scanned) is known to contain no newline
  char chunk[64 * 1024];
  bool open = true;
  while (open) {
    ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;  // client closed, or Stop() shut down our read side
    buffer.append(chunk, static_cast<size_t>(n));
    size_t start = 0, nl;
    // Search only the new bytes, so a long line arriving in many chunks costs O(length).
    for (size_t from = scanned; open && (nl = buffer.find('\n', from)) != std::string::npos;
         start = from = nl + 1) {
      std::string_view line(buffer.data() + start, nl - start);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (line.empty()) continue;
      open = SendAll(fd, handler_->HandleLine(line) + "\n");
    }
    buffer.erase(0, start);
    scanned = buffer.size();
    if (buffer.size() > kMaxLineBytes) {
      SendAll(fd, ErrorLine(Status::InvalidArgument("request line longer than 17 MiB")));
      break;
    }
  }
  std::lock_guard<std::mutex> l(mu_);
  conns_.erase(fd);
  ::close(fd);
  active_--;
  all_done_.notify_all();  // under the lock: Stop() may destroy *this right after waking
}

void TcpServer::Stop() {
  if (stopping_.exchange(true)) return;
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);  // wakes the blocked accept()
    if (accept_thread_.joinable()) accept_thread_.join();
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  std::unique_lock<std::mutex> l(mu_);
  // Closing only the read side lets a request that is being processed still send its reply;
  // the connection thread then sees end-of-file and exits.
  for (int fd : conns_) ::shutdown(fd, SHUT_RD);
  all_done_.wait(l, [&] { return active_ == 0; });
}

}  // namespace docstore
