// docstore_server --db <path> [--host 127.0.0.1] [--port 7070] [--max-conns 256]
//
// Serves newline-delimited JSON requests (see src/server/command_handler.h). SIGINT/SIGTERM
// shut it down gracefully: stop accepting, finish in-flight requests, close the database.

#include <pthread.h>
#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "docstore/database.h"
#include "server/command_handler.h"
#include "server/tcp_server.h"

int main(int argc, char** argv) {
  std::string db_path, host = "127.0.0.1";
  int port = 7070, max_conns = 256;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", arg.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--db")
      db_path = next();
    else if (arg == "--host")
      host = next();
    else if (arg == "--port")
      port = std::atoi(next().c_str());
    else if (arg == "--max-conns")
      max_conns = std::atoi(next().c_str());
    else {
      std::fprintf(stderr, "unknown flag %s\n", arg.c_str());
      return 2;
    }
  }
  if (db_path.empty()) {
    std::fprintf(stderr, "usage: %s --db <path> [--host H] [--port P] [--max-conns N]\n", argv[0]);
    return 2;
  }

  // Block the shutdown signals in every thread (threads inherit the mask); main waits for
  // them with sigwait, so no handler runs in an arbitrary thread.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);

  std::unique_ptr<docstore::Database> db;
  docstore::Status s = docstore::Database::Open(docstore::Options(), db_path, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "cannot open %s: %s\n", db_path.c_str(), s.ToString().c_str());
    return 1;
  }
  docstore::CommandHandler handler(db.get());
  docstore::TcpServer server(&handler, max_conns);
  s = server.Start(host, port);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("docstore_server listening on %s:%d (db %s)\n", host.c_str(), server.port(),
              db_path.c_str());
  std::fflush(stdout);

  int sig = 0;
  sigwait(&signals, &sig);
  std::printf("received %s, shutting down\n", sig == SIGINT ? "SIGINT" : "SIGTERM");
  server.Stop();
  s = db->Close();
  std::printf("closed: %s\n", s.ToString().c_str());
  return s.ok() ? 0 : 1;
}
