#include "mock_platform.h"

#include "fleet_proto.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace mock {

namespace {

// ---- SHA-1 + base64 for Sec-WebSocket-Accept -------------------------------------------------

struct Sha1 {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  static uint32_t Rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }
  void Block(const unsigned char* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
    for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      const uint32_t t = Rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = Rol(b, 30);
      b = a;
      a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
  }
  std::string Digest(const std::string& msg) {
    std::string m = msg;
    const uint64_t bits = uint64_t(msg.size()) * 8;
    m.push_back(char(0x80));
    while (m.size() % 64 != 56) m.push_back('\0');
    for (int i = 7; i >= 0; --i) m.push_back(char((bits >> (8 * i)) & 0xFF));
    for (size_t i = 0; i < m.size(); i += 64) Block(reinterpret_cast<const unsigned char*>(m.data() + i));
    std::string out;
    for (uint32_t v : h) {
      for (int i = 3; i >= 0; --i) out.push_back(char((v >> (8 * i)) & 0xFF));
    }
    return out;
  }
};

std::string Base64(const std::string& in) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  while (i + 2 < in.size()) {
    const uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8) | uint8_t(in[i + 2]);
    out += t[v >> 18];
    out += t[(v >> 12) & 63];
    out += t[(v >> 6) & 63];
    out += t[v & 63];
    i += 3;
  }
  if (i + 1 == in.size()) {
    const uint32_t v = uint32_t(uint8_t(in[i])) << 16;
    out += t[v >> 18];
    out += t[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8);
    out += t[v >> 18];
    out += t[(v >> 12) & 63];
    out += t[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

bool WriteAll(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    const ssize_t w = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (w <= 0) return false;
    off += size_t(w);
  }
  return true;
}

// Reads exactly n bytes; false on EOF/error/stop.
bool ReadExact(int fd, std::string* out, size_t n, const std::atomic<bool>& stop) {
  out->clear();
  while (out->size() < n) {
    pollfd p{fd, POLLIN, 0};
    const int r = poll(&p, 1, 100);
    if (stop) return false;
    if (r <= 0) continue;
    char buf[65536];
    const ssize_t got = recv(fd, buf, std::min(sizeof(buf), n - out->size()), 0);
    if (got <= 0) return false;
    out->append(buf, size_t(got));
  }
  return true;
}

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Response(int status, const char* reason, const std::string& body) {
  return "HTTP/1.1 " + std::to_string(status) + " " + reason +
         "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\n\r\n" + body;
}

}  // namespace

bool Platform::Start(int port) {
  listenFd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listenFd_ < 0) return false;
  int one = 1;
  setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(uint16_t(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listenFd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(listenFd_, 16) != 0) {
    close(listenFd_);
    listenFd_ = -1;
    return false;
  }
  socklen_t len = sizeof(a);
  getsockname(listenFd_, reinterpret_cast<sockaddr*>(&a), &len);
  port_ = ntohs(a.sin_port);
  stop_ = false;
  acceptThread_ = std::thread([this] { AcceptLoop(); });
  return true;
}

void Platform::Stop() {
  if (listenFd_ < 0) return;
  stop_ = true;
  shutdown(listenFd_, SHUT_RDWR);
  if (acceptThread_.joinable()) acceptThread_.join();
  close(listenFd_);
  listenFd_ = -1;
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();
}

void Platform::AcceptLoop() {
  while (!stop_) {
    pollfd p{listenFd_, POLLIN, 0};
    if (poll(&p, 1, 100) <= 0) continue;
    const int fd = accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) continue;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    workers_.emplace_back([this, fd] { Serve(fd); });
  }
}

void Platform::Serve(int fd) {
  std::string head;
  char c;
  while (head.size() < 16384 && head.find("\r\n\r\n") == std::string::npos) {
    std::string one;
    if (!ReadExact(fd, &one, 1, stop_)) {
      close(fd);
      return;
    }
    c = one[0];
    head.push_back(c);
  }
  const size_t lineEnd = head.find("\r\n");
  const std::string requestLine = head.substr(0, lineEnd);
  std::map<std::string, std::string> hdr;
  size_t pos = lineEnd + 2;
  while (pos < head.size()) {
    const size_t e = head.find("\r\n", pos);
    if (e == std::string::npos || e == pos) break;
    const std::string line = head.substr(pos, e - pos);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string v = line.substr(colon + 1);
      while (!v.empty() && v[0] == ' ') v.erase(0, 1);
      hdr[Lower(line.substr(0, colon))] = v;
    }
    pos = e + 2;
  }
  if (requestLine.rfind("POST /api/fleet/enroll", 0) == 0) {
    std::string body;
    const size_t n = hdr.count("content-length") ? size_t(std::atol(hdr["content-length"].c_str())) : 0;
    ReadExact(fd, &body, n, stop_);
    ++enrollments_;
    {
      std::lock_guard<std::mutex> lk(mu_);
      enrollBody_ = body;
    }
    fleet::json::Value v;
    fleet::json::Parse(body, &v);
    const std::string code = v.Get("code") ? v.Get("code")->AsStr() : "";
    if (code == "RUE-BAD0-BAD0-BAD0") {
      WriteAll(fd, Response(403, "Forbidden", "{\"error\":\"invalid or used code\"}"));
    } else {
      const std::string resp = "{\"server_id\":\"" + serverId + "\",\"token\":\"" + token +
                               "\",\"ws_url\":\"ws://127.0.0.1:" + std::to_string(port_) + "/api/fleet/ws\"}";
      WriteAll(fd, Response(201, "Created", resp));
    }
    if (verbose) std::printf("[mock] enroll: %s\n", fleet::Redact(body).c_str());
    close(fd);
    return;
  }
  if (requestLine.rfind("GET /api/fleet/ws", 0) == 0 && Lower(hdr["upgrade"]) == "websocket") {
    {
      std::lock_guard<std::mutex> lk(mu_);
      authHeader_ = hdr["authorization"];
    }
    if (const int st = rejectUpgradeStatus.load()) {
      WriteAll(fd, Response(st, "Refused", "{}"));
      close(fd);
      return;
    }
    if (hdr["authorization"] != "Bearer " + token) {
      WriteAll(fd, Response(401, "Unauthorized", "{\"error\":\"bad token\"}"));
      close(fd);
      return;
    }
    const std::string accept =
        Base64(Sha1().Digest(hdr["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    WriteAll(fd, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                     accept + "\r\n\r\n");
    const int conn = ++connections_;
    ServeWs(fd, conn);
    return;
  }
  WriteAll(fd, Response(404, "Not Found", "{}"));
  close(fd);
}

bool Platform::WriteFrame(int fd, int opcode, const std::string& payload) {
  std::string f;
  f.push_back(char(0x80 | opcode));
  if (payload.size() < 126) {
    f.push_back(char(payload.size()));
  } else if (payload.size() < 65536) {
    f.push_back(char(126));
    f.push_back(char((payload.size() >> 8) & 0xFF));
    f.push_back(char(payload.size() & 0xFF));
  } else {
    f.push_back(char(127));
    for (int i = 7; i >= 0; --i) f.push_back(char((uint64_t(payload.size()) >> (8 * i)) & 0xFF));
  }
  f += payload;
  std::lock_guard<std::mutex> lk(writeMu_);
  return WriteAll(fd, f);
}

bool Platform::SendEnvelope(int fd, const std::string& type, const std::string& payloadJson, int64_t seq,
                            const std::string& ref) {
  fleet::Envelope e;
  e.type = type;
  e.id = fleet::NewUlid(fleet::NowMs());
  e.seq = seq;
  e.ts = fleet::NowMs();
  e.ref = ref;
  {
    std::lock_guard<std::mutex> lk(mu_);
    e.ack = rxByStream_.count(currentStream_) ? rxByStream_[currentStream_] : 0;
  }
  const std::string text = fleet::EncodeRaw(e, payloadJson);
  if (verbose) std::printf("[mock] -> %s\n", text.c_str());
  return WriteFrame(fd, 1, text);
}

void Platform::ServeWs(int fd, int conn) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    currentFd_ = fd;
  }
  std::string msg;
  int msgOpcode = 0;
  while (!stop_) {
    std::string h;
    if (!ReadExact(fd, &h, 2, stop_)) break;
    const bool fin = (uint8_t(h[0]) & 0x80) != 0;
    const int opcode = uint8_t(h[0]) & 0x0F;
    const bool masked = (uint8_t(h[1]) & 0x80) != 0;
    uint64_t len = uint8_t(h[1]) & 0x7F;
    std::string ext;
    if (len == 126) {
      if (!ReadExact(fd, &ext, 2, stop_)) break;
      len = (uint64_t(uint8_t(ext[0])) << 8) | uint8_t(ext[1]);
    } else if (len == 127) {
      if (!ReadExact(fd, &ext, 8, stop_)) break;
      len = 0;
      for (int i = 0; i < 8; ++i) len = (len << 8) | uint8_t(ext[i]);
    }
    std::string mask, payload;
    if (masked && !ReadExact(fd, &mask, 4, stop_)) break;
    if (!ReadExact(fd, &payload, size_t(len), stop_)) break;
    if (masked) {
      for (size_t i = 0; i < payload.size(); ++i) payload[i] = char(payload[i] ^ mask[i % 4]);
    }
    if (opcode == 8) {  // close
      WriteFrame(fd, 8, payload.substr(0, 2));
      break;
    }
    if (opcode == 9) {
      WriteFrame(fd, 10, payload);
      continue;
    }
    if (opcode == 10) continue;
    if (opcode != 0) msgOpcode = opcode;
    msg += payload;
    if (!fin) continue;
    const std::string text = std::move(msg);
    msg.clear();
    if (msgOpcode != 1) continue;
    if (verbose) std::printf("[mock] <- %s\n", fleet::Redact(text).c_str());
    fleet::json::Value env;
    if (!fleet::json::Parse(text, &env) || !env.IsObj()) continue;
    Received r;
    r.conn = conn;
    r.env = env;
    const std::string type = r.type();
    const int64_t seq = r.seq();
    if (const fleet::json::Value* a = env.Get("ack"); a && a->t == fleet::json::Value::T::Int) {
      lastAckFromServer_ = a->i;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      msgs_.push_back(r);
    }
    cv_.notify_all();
    if (silent) continue;
    const fleet::json::Value* p = env.Get("payload");
    if (type == "hello") {
      std::string stream;
      if (p && p->Get("stream") && p->Get("stream")->Get("id")) stream = p->Get("stream")->Get("id")->AsStr();
      bool known;
      int64_t rx;
      {
        std::lock_guard<std::mutex> lk(mu_);
        currentStream_ = stream;
        known = !forgetStreams && rxByStream_.count(stream) > 0;
        if (!known) rxByStream_[stream] = 0;
        rx = rxByStream_[stream];
      }
      const std::string welcome = "{\"session_id\":\"sess_" + std::to_string(conn) +
                                  "\",\"protocol\":1,\"heartbeat\":{\"interval_ms\":" +
                                  std::to_string(heartbeatIntervalMs.load()) +
                                  ",\"timeout_ms\":" + std::to_string(heartbeatTimeoutMs.load()) +
                                  "},\"resume\":{\"result\":\"" + (known ? "resumed" : "reset") +
                                  "\",\"platform_last_rx_seq\":" + std::to_string(rx) +
                                  "},\"server_config_rev\":0,\"admins_rev\":0,\"assignment\":null}";
      SendEnvelope(fd, "welcome", welcome, 0, "");
      continue;
    }
    if (type == "ping") {
      std::string t = "0";
      if (p && p->Get("t")) t = std::to_string(p->Get("t")->AsInt(0));
      SendEnvelope(fd, "pong", "{\"t\":" + t + "}", 0, env.Get("id") ? env.Get("id")->AsStr() : "");
      continue;
    }
    if (seq > 0) {
      bool ack = false;
      {
        std::lock_guard<std::mutex> lk(mu_);
        int64_t& rx = rxByStream_[currentStream_];
        if (seq == rx + 1) rx = seq;
        ack = autoAck;
      }
      if (ack) SendEnvelope(fd, "ack", "{}", 0, "");
    }
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (currentFd_ == fd) currentFd_ = -1;
  }
  close(fd);
}

bool Platform::SendToServer(const std::string& type, const std::string& payloadJson, bool reliable) {
  int fd;
  int64_t seq = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    fd = currentFd_;
    if (reliable) seq = ++txSeq_;
  }
  if (fd < 0) return false;
  return SendEnvelope(fd, type, payloadJson, seq, "");
}

bool Platform::SendRaw(const std::string& text) {
  int fd;
  {
    std::lock_guard<std::mutex> lk(mu_);
    fd = currentFd_;
  }
  return fd >= 0 && WriteFrame(fd, 1, text);
}

void Platform::CloseCurrent(int code, const std::string& reason) {
  int fd;
  {
    std::lock_guard<std::mutex> lk(mu_);
    fd = currentFd_;
  }
  if (fd < 0) return;
  std::string p;
  p.push_back(char((code >> 8) & 0xFF));
  p.push_back(char(code & 0xFF));
  p += reason;
  WriteFrame(fd, 8, p);
  shutdown(fd, SHUT_RDWR);
}

void Platform::DropCurrent() {
  std::lock_guard<std::mutex> lk(mu_);
  if (currentFd_ >= 0) shutdown(currentFd_, SHUT_RDWR);
}

void Platform::SetPlatformRxSeq(const std::string& streamId, int64_t seq) {
  std::lock_guard<std::mutex> lk(mu_);
  rxByStream_[streamId] = seq;
}

std::vector<Received> Platform::Messages() {
  std::lock_guard<std::mutex> lk(mu_);
  return msgs_;
}

std::vector<Received> Platform::MessagesOfType(const std::string& type) {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<Received> out;
  for (const auto& m : msgs_) {
    if (m.type() == type) out.push_back(m);
  }
  return out;
}

std::string Platform::lastEnrollBody() {
  std::lock_guard<std::mutex> lk(mu_);
  return enrollBody_;
}

std::string Platform::lastAuthHeader() {
  std::lock_guard<std::mutex> lk(mu_);
  return authHeader_;
}

int64_t Platform::platformRxSeq(const std::string& streamId) {
  std::lock_guard<std::mutex> lk(mu_);
  return rxByStream_.count(streamId) ? rxByStream_[streamId] : -1;
}

bool Platform::WaitFor(const std::function<bool()>& pred, int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(20));
  }
  return pred();
}

}  // namespace mock
