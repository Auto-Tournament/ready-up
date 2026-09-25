// A tiny fake Auto Tournament platform for tests: POST /api/fleet/enroll and the
// /api/fleet/ws WebSocket (plain HTTP/WS on 127.0.0.1, RFC 6455 framing, no TLS). It speaks
// just enough of docs/FLEET.md §4-§6 to drive fleet.so: hello -> welcome with resume, seq/ack,
// ping/pong, platform -> server messages, and scripted closes / drops / silence.
#pragma once

#include "fleet_json.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mock {

struct Received {
  int conn = 0;  // connection number (1-based)
  fleet::json::Value env;
  std::string type() const { return env.Get("type") ? env.Get("type")->AsStr() : ""; }
  int64_t seq() const { return env.Get("seq") ? env.Get("seq")->AsInt(0) : 0; }
  const fleet::json::Value* payload() const { return env.Get("payload"); }
};

class Platform {
 public:
  Platform() = default;
  ~Platform() { Stop(); }

  bool Start(int port = 0);  // 0 = ephemeral
  void Stop();
  int port() const { return port_; }
  std::string BaseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

  // ---- knobs (set before or during a test) ----
  std::string token = "rus_t0k3nt0k3nt0_c2VjcmV0LXNlY3JldC1zZWNyZXQtc2VjcmV0LXNlY3J";
  std::string serverId = "srv_test_1";
  std::atomic<bool> autoAck{true};        // ack every reliable message right away
  std::atomic<bool> silent{false};        // stop answering anything (heartbeat timeout test)
  std::atomic<int> heartbeatIntervalMs{10000};
  std::atomic<int> heartbeatTimeoutMs{30000};
  std::atomic<bool> forgetStreams{false}; // pretend we never saw any stream (resume -> reset)
  std::atomic<int> rejectUpgradeStatus{0};// e.g. 401: refuse the WS upgrade

  // ---- controls ----
  // Sends a platform -> server message on the current connection. reliable = gets a seq.
  bool SendToServer(const std::string& type, const std::string& payloadJson, bool reliable);
  bool SendRaw(const std::string& text);
  // Close frame with a code; or drop the TCP connection without one.
  void CloseCurrent(int code, const std::string& reason = {});
  void DropCurrent();
  // Pretend the platform only durably received up to `seq` of stream `streamId`.
  void SetPlatformRxSeq(const std::string& streamId, int64_t seq);

  // ---- observations ----
  std::vector<Received> Messages();
  std::vector<Received> MessagesOfType(const std::string& type);
  int connections() const { return connections_.load(); }
  int enrollments() const { return enrollments_.load(); }
  std::string lastEnrollBody();
  std::string lastAuthHeader();
  int64_t platformRxSeq(const std::string& streamId);
  int64_t lastAckFromServer() const { return lastAckFromServer_.load(); }
  // Waits until pred() is true or timeoutMs passes. Returns pred().
  bool WaitFor(const std::function<bool()>& pred, int timeoutMs);

  // Prints every frame to stdout (standalone mock server).
  bool verbose = false;

 private:
  void AcceptLoop();
  void Serve(int fd);
  void ServeWs(int fd, int conn);
  bool WriteFrame(int fd, int opcode, const std::string& payload);
  bool SendEnvelope(int fd, const std::string& type, const std::string& payloadJson, int64_t seq,
                    const std::string& ref);

  int listenFd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread acceptThread_;
  std::vector<std::thread> workers_;

  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<Received> msgs_;
  std::map<std::string, int64_t> rxByStream_;  // highest contiguous server seq per stream
  std::string currentStream_;
  int currentFd_ = -1;
  int64_t txSeq_ = 0;  // platform -> server
  std::string enrollBody_, authHeader_;
  std::atomic<int> connections_{0};
  std::atomic<int> enrollments_{0};
  std::atomic<int64_t> lastAckFromServer_{-1};
  std::mutex writeMu_;
};

}  // namespace mock
