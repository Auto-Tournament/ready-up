#pragma once
// Local status endpoint (docs/FLEET.md §17): the HTTP server.
//
// One background thread, one poll() loop, non-blocking sockets. It only reads from the Hub
// (status_snapshot.h); it never calls into the game thread or the engine, and the game thread
// never waits on it. Engine-free, so tests/status_http_test.cpp runs it on an ephemeral port.
//
// Routes (GET and HEAD only, anything else 405):
//   /health    no auth. 200 {"ok":true,...} or 503 when the engine surface is disabled or the
//              last selftest failed.
//   /status    JSON (docs/FLEET.md §17.2)
//   /stream    Server-Sent Events: `snapshot`, then `patch` {rev, patch} and `status` events,
//              `: keepalive` every 15 s; Last-Event-ID resumes from the ring
//   /metrics   Prometheus text (only when enabled)
//   /selftest  the last selftest report (text); `?run=1` queues a new run on the game thread
// Everything but /health needs the token unless the peer is loopback.
//
// Limits: max connections (extra ones get a 503 and are closed), max concurrent streams,
// header size, read timeout, per-IP token bucket (429), and a per-stream write buffer cap
// (a stream that cannot keep up is dropped and reconnects).
#include "readyup/status_snapshot.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace readyup::status {

struct ServerConfig {
  std::string bind = "127.0.0.1";
  int port = 0;  // 0 = ephemeral (tests)
  std::string token;
  bool metrics = false;
  bool trustLoopback = true;  // loopback peers need no token

  int maxConnections = 16;
  int maxStreams = 8;
  size_t maxHeaderBytes = 8 * 1024;
  int readTimeoutMs = 5000;
  int writeTimeoutMs = 10000;  // plain responses
  size_t streamMaxBuffered = 256 * 1024;
  int keepaliveMs = 15000;
  int pollMs = 100;  // also the max latency between a Submit() and stream delivery
  double ratePerSec = 5.0;
  double rateBurst = 20.0;
  int selftestTriggerMinIntervalMs = 30000;

  // Called on the HTTP thread for `/selftest?run=1`; must only set a flag (the game thread
  // runs it). Null = triggering disabled.
  std::function<void()> requestSelftest;
  // Log sink (any thread). Null = silent.
  std::function<void(const std::string&)> log;
};

// True for 127.0.0.0/8, ::1 and ::ffff:127.x.
bool IsLoopbackAddress(const std::string& ip);

class StatusServer {
 public:
  StatusServer(ServerConfig cfg, std::shared_ptr<Hub> hub);
  ~StatusServer();
  StatusServer(const StatusServer&) = delete;
  StatusServer& operator=(const StatusServer&) = delete;

  // Binds and starts the thread. False (with *err) if the socket could not be bound or the
  // config is unsafe (non-loopback bind without a token).
  bool Start(std::string* err);
  void Stop();
  bool Running() const { return running_.load(); }
  int BoundPort() const { return boundPort_; }

  struct Counters {
    std::atomic<uint64_t> requests{0};
    std::atomic<uint64_t> rateLimited{0};
    std::atomic<uint64_t> unauthorized{0};
    std::atomic<uint64_t> rejectedConnections{0};
    std::atomic<uint64_t> streamsDropped{0};
    std::atomic<uint64_t> streamsActive{0};
    std::atomic<uint64_t> connectionsActive{0};
    std::atomic<uint64_t> loopIterationsMaxUs{0};
  };
  const Counters& counters() const { return counters_; }

 private:
  struct Impl;
  void Run();

  ServerConfig cfg_;
  std::shared_ptr<Hub> hub_;
  std::unique_ptr<Impl> impl_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  int listenFd_ = -1;
  int wakeFds_[2] = {-1, -1};
  int boundPort_ = 0;
  Counters counters_;
};

}  // namespace readyup::status
