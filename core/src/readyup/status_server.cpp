#include "readyup/status_server.h"

#include "readyup/status_http.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <vector>

namespace readyup::status {
namespace {

using Clock = std::chrono::steady_clock;

double NowSec() {
  return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

long long UnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void SetNonBlockingCloexec(int fd) {
  const int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  const int fdfl = fcntl(fd, F_GETFD, 0);
  if (fdfl >= 0) fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
}

std::string PeerIp(const sockaddr_storage& ss) {
  char buf[INET6_ADDRSTRLEN] = {0};
  if (ss.ss_family == AF_INET) {
    const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
    inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
  } else if (ss.ss_family == AF_INET6) {
    const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
    inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
  }
  return buf;
}

// Pulls one number out of summary / selftest JSON for /metrics.
bool NumAt(const Json& root, std::initializer_list<const char*> path, double* out) {
  const Json* j = &root;
  for (const char* k : path) {
    j = j->Find(k);
    if (!j) return false;
  }
  switch (j->type()) {
    case Json::Type::Int:
    case Json::Type::Double:
    case Json::Type::Bool:
      *out = static_cast<double>(j->type() == Json::Type::Bool ? (j->AsBool() ? 1 : 0) : j->AsInt());
      return true;
    default:
      return false;
  }
}

std::string FmtNum(double v) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.10g", v);
  return buf;
}

}  // namespace

bool IsLoopbackAddress(const std::string& ip) {
  if (ip == "::1") return true;
  if (ip.rfind("127.", 0) == 0) return true;
  if (ip.rfind("::ffff:127.", 0) == 0) return true;
  return false;
}

struct StatusServer::Impl {
  enum class St { Reading, Writing, Streaming };
  struct Conn {
    int fd = -1;
    std::string ip;
    bool loopback = false;
    St st = St::Reading;
    std::string in;
    std::string out;
    size_t outOff = 0;
    double deadline = 0;       // Reading / Writing
    double lastKeepalive = 0;  // Streaming
    uint64_t seq = 0;          // Streaming: last hub event delivered
    bool dead = false;
  };
  std::vector<Conn> conns;
  RateLimiter limiter;
  std::map<int, uint64_t> byCode;
  double startedAt = NowSec();
  double lastSelftestTrigger = -1e18;
  int streams = 0;

  Impl(double rate, double burst) : limiter(rate, burst, 1024) {}
};

StatusServer::StatusServer(ServerConfig cfg, std::shared_ptr<Hub> hub)
    : cfg_(std::move(cfg)), hub_(std::move(hub)), impl_(new Impl(cfg_.ratePerSec, cfg_.rateBurst)) {}

StatusServer::~StatusServer() { Stop(); }

bool StatusServer::Start(std::string* err) {
  if (running_.load()) return true;
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    if (listenFd_ >= 0) close(listenFd_);
    listenFd_ = -1;
    return false;
  };
  if (!hub_) return fail("no hub");

  // Resolve the bind address (numeric only; no DNS on the server).
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE | AI_NUMERICSERV;
  addrinfo* res = nullptr;
  const std::string portStr = std::to_string(cfg_.port);
  const int gai = getaddrinfo(cfg_.bind.empty() ? nullptr : cfg_.bind.c_str(), portStr.c_str(), &hints, &res);
  if (gai != 0 || !res) return fail("bad bind address \"" + cfg_.bind + "\" (numeric IPv4/IPv6 only)");

  // Loopback bind? (0.0.0.0 / :: are not.)
  bool loopbackBind = false;
  {
    sockaddr_storage ss{};
    std::memcpy(&ss, res->ai_addr, std::min<size_t>(res->ai_addrlen, sizeof(ss)));
    loopbackBind = IsLoopbackAddress(PeerIp(ss));
  }
  if (!loopbackBind && cfg_.token.size() < 16) {
    freeaddrinfo(res);
    return fail("refusing to listen on non-loopback " + cfg_.bind + " without a status_http_token (>= 16 chars)");
  }

  listenFd_ = socket(res->ai_family, SOCK_STREAM, 0);
  if (listenFd_ < 0) {
    freeaddrinfo(res);
    return fail(std::string("socket: ") + std::strerror(errno));
  }
  int one = 1;
  setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (bind(listenFd_, res->ai_addr, res->ai_addrlen) != 0) {
    const int e = errno;
    freeaddrinfo(res);
    return fail("bind " + cfg_.bind + ":" + portStr + ": " + std::strerror(e));
  }
  freeaddrinfo(res);
  if (listen(listenFd_, 32) != 0) return fail(std::string("listen: ") + std::strerror(errno));
  SetNonBlockingCloexec(listenFd_);
  {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (getsockname(listenFd_, reinterpret_cast<sockaddr*>(&ss), &len) == 0) {
      boundPort_ = ntohs(ss.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port
                                                  : reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
    }
  }
  if (pipe(wakeFds_) != 0) return fail(std::string("pipe: ") + std::strerror(errno));
  SetNonBlockingCloexec(wakeFds_[0]);
  SetNonBlockingCloexec(wakeFds_[1]);

  stop_.store(false);
  running_.store(true);
  impl_->startedAt = NowSec();
  thread_ = std::thread([this] { Run(); });
  return true;
}

void StatusServer::Stop() {
  if (!thread_.joinable()) return;
  stop_.store(true);
  if (wakeFds_[1] >= 0) {
    const char b = 1;
    (void)!write(wakeFds_[1], &b, 1);
  }
  thread_.join();
  running_.store(false);
  for (auto& c : impl_->conns) {
    if (c.fd >= 0) close(c.fd);
  }
  impl_->conns.clear();
  if (listenFd_ >= 0) close(listenFd_);
  listenFd_ = -1;
  for (int& fd : wakeFds_) {
    if (fd >= 0) close(fd);
    fd = -1;
  }
}

void StatusServer::Run() {
  Impl& im = *impl_;
  using Conn = Impl::Conn;
  using St = Impl::St;

  auto log = [this](const std::string& s) {
    if (cfg_.log) cfg_.log(s);
  };
  auto count = [&](int code) {
    counters_.requests.fetch_add(1, std::memory_order_relaxed);
    im.byCode[code]++;
  };
  auto respond = [&](Conn& c, int code, const std::string& ctype, const std::string& body, bool head,
                     const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    count(code);
    c.out = BuildResponse(code, ctype, body, head, extra);
    c.outOff = 0;
    c.st = St::Writing;
    c.deadline = NowSec() + cfg_.writeTimeoutMs / 1000.0;
  };
  auto jsonErr = [](const std::string& msg) {
    std::string b = "{\"error\":";
    JsonEscapeTo(b, msg);
    b += "}\n";
    return b;
  };

  auto metricsBody = [&]() {
    const double now = NowSec();
    std::string m;
    auto g = [&m](const std::string& name, const std::string& help, double v, const char* type = "gauge") {
      m += "# HELP " + name + " " + help + "\n# TYPE " + name + " " + type + "\n" + name + " " + FmtNum(v) + "\n";
    };
    g("readyup_up", "Ready Up status endpoint is serving.", 1);
    g("readyup_status_uptime_seconds", "Seconds since the status endpoint started.", now - im.startedAt);
    g("readyup_status_rev", "Current MatchState revision.", static_cast<double>(hub_->Rev()));
    auto cur = hub_->Current();
    if (cur) {
      g("readyup_update_safe", "1 when a game/plugin update or restart would not interrupt a match.",
        cur->update_safe ? 1 : 0);
      g("readyup_healthy", "0 when the engine surface is disabled or the last selftest failed.", cur->healthy ? 1 : 0);
      double v = 0;
      if (NumAt(cur->selftest, {"pass"}, &v)) g("readyup_selftest_pass", "Last selftest passed.", v);
      if (NumAt(cur->selftest, {"passed"}, &v)) g("readyup_selftest_passed", "Checks passed in the last selftest.", v);
      if (NumAt(cur->selftest, {"total"}, &v)) g("readyup_selftest_total", "Checks in the last selftest.", v);
      if (NumAt(cur->summary, {"players", "connected"}, &v)) g("readyup_players_connected", "Connected humans.", v);
      if (NumAt(cur->summary, {"players", "expected"}, &v)) g("readyup_players_expected", "Roster size of the loaded match.", v);
      if (NumAt(cur->summary, {"round"}, &v)) g("readyup_round", "Current round number.", v);
      if (NumAt(cur->summary, {"map_number"}, &v)) g("readyup_map_number", "Current map of the series (1-based).", v);
      double s1 = 0, s2 = 0;
      if (NumAt(cur->summary, {"score", "team1"}, &s1) && NumAt(cur->summary, {"score", "team2"}, &s2)) {
        m += "# HELP readyup_score Map score.\n# TYPE readyup_score gauge\n";
        m += "readyup_score{team=\"team1\"} " + FmtNum(s1) + "\nreadyup_score{team=\"team2\"} " + FmtNum(s2) + "\n";
      }
      const Json* st = cur->platform.Find("state");
      const bool online = st && st->type() == Json::Type::String && st->AsString() == "online";
      g("readyup_platform_online", "1 while the fleet connection is online.", online ? 1 : 0);
      const Json* mode = cur->summary.Find("mode");
      if (mode && mode->type() == Json::Type::String) {
        m += "# HELP readyup_mode Current Ready Up mode.\n# TYPE readyup_mode gauge\n";
        for (const char* k : {"idle", "scrim", "match", "practice", "external"}) {
          m += std::string("readyup_mode{mode=\"") + k + "\"} " + (mode->AsString() == k ? "1" : "0") + "\n";
        }
      }
      for (const auto& kv : cur->gauges) g("readyup_" + kv.first, kv.first, kv.second);
    }
    m += "# HELP readyup_http_requests_total HTTP requests by status code.\n# TYPE readyup_http_requests_total counter\n";
    for (const auto& kv : im.byCode) {
      m += "readyup_http_requests_total{code=\"" + std::to_string(kv.first) + "\"} " + std::to_string(kv.second) + "\n";
    }
    g("readyup_http_rate_limited_total", "Requests refused with 429.",
      static_cast<double>(counters_.rateLimited.load()), "counter");
    g("readyup_http_connections_rejected_total", "Connections refused at the connection cap.",
      static_cast<double>(counters_.rejectedConnections.load()), "counter");
    g("readyup_http_streams", "Open /stream clients.", im.streams);
    g("readyup_http_streams_dropped_total", "Streams dropped for not keeping up.",
      static_cast<double>(counters_.streamsDropped.load()), "counter");
    return m;
  };

  auto handle = [&](Conn& c, const HttpRequest& req) {
    const bool head = req.method == "HEAD";
    if (req.method != "GET" && !head) {
      respond(c, 405, "application/json", jsonErr("only GET and HEAD"), false, {{"Allow", "GET, HEAD"}});
      return;
    }
    if (!im.limiter.Allow(c.ip, NowSec())) {
      counters_.rateLimited.fetch_add(1, std::memory_order_relaxed);
      respond(c, 429, "application/json", jsonErr("rate limited"), head, {{"Retry-After", "1"}});
      return;
    }
    const long long uptime = static_cast<long long>(NowSec() - im.startedAt);
    const std::string& p = req.path;

    if (p == "/health") {
      auto cur = hub_->Current();
      const bool ok = cur && cur->healthy;
      std::string b = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"uptime_s\":" + std::to_string(uptime);
      if (!ok) {
        b += ",\"reason\":";
        JsonEscapeTo(b, cur ? cur->unhealthy_reason : std::string("starting"));
      }
      b += "}\n";
      respond(c, ok ? 200 : 503, "application/json", b, head);
      return;
    }
    if (p == "/") {
      respond(c, 200, "application/json",
              "{\"endpoints\":[\"/health\",\"/status\",\"/stream\",\"/metrics\",\"/selftest\"]}\n", head);
      return;
    }
    const bool known = p == "/status" || p == "/stream" || p == "/metrics" || p == "/selftest";
    if (!known) {
      respond(c, 404, "application/json", jsonErr("not found"), head);
      return;
    }
    // Auth: loopback peers are trusted; everyone else needs the token.
    if (!(cfg_.trustLoopback && c.loopback)) {
      const std::string tok = ExtractToken(req);
      if (cfg_.token.empty() || !TokenEquals(tok, cfg_.token)) {
        counters_.unauthorized.fetch_add(1, std::memory_order_relaxed);
        respond(c, 401, "application/json", jsonErr("token required"), head,
                {{"WWW-Authenticate", "Bearer realm=\"readyup-status\""}});
        return;
      }
    }

    if (p == "/status") {
      respond(c, 200, "application/json", hub_->StatusBody(uptime, UnixMs()) + "\n", head);
      return;
    }
    if (p == "/metrics") {
      if (!cfg_.metrics) {
        respond(c, 404, "application/json", jsonErr("metrics disabled (status_http_metrics 1)"), head);
        return;
      }
      respond(c, 200, "text/plain; version=0.0.4; charset=utf-8", metricsBody(), head);
      return;
    }
    if (p == "/selftest") {
      bool runPresent = false;
      const std::string run = req.QueryParam("run", &runPresent);
      if (runPresent && run != "0" && !head) {
        if (!cfg_.requestSelftest) {
          respond(c, 403, "application/json", jsonErr("selftest trigger disabled"), false);
          return;
        }
        const double now = NowSec();
        if ((now - im.lastSelftestTrigger) * 1000.0 < cfg_.selftestTriggerMinIntervalMs) {
          respond(c, 429, "application/json", jsonErr("selftest was triggered recently"), false,
                  {{"Retry-After", std::to_string(cfg_.selftestTriggerMinIntervalMs / 1000)}});
          return;
        }
        im.lastSelftestTrigger = now;
        cfg_.requestSelftest();
        respond(c, 202, "application/json",
                "{\"queued\":true,\"note\":\"runs on the next server frame; GET /selftest for the report\"}\n", false);
        return;
      }
      auto cur = hub_->Current();
      if (!cur || cur->selftest_report.empty()) {
        respond(c, 200, "text/plain; charset=utf-8", "selftest: not run yet\n", head);
      } else {
        respond(c, 200, "text/plain; charset=utf-8", cur->selftest_report + "\n", head);
      }
      return;
    }
    // /stream
    if (head) {
      count(200);
      c.out = BuildSseHead();
      c.outOff = 0;
      c.st = St::Writing;
      c.deadline = NowSec() + cfg_.writeTimeoutMs / 1000.0;
      return;
    }
    if (im.streams >= cfg_.maxStreams) {
      respond(c, 503, "application/json", jsonErr("too many streams"), false, {{"Retry-After", "5"}});
      return;
    }
    count(200);
    im.streams++;
    counters_.streamsActive.store(static_cast<uint64_t>(im.streams));
    c.st = St::Streaming;
    c.out = BuildSseHead();
    c.outOff = 0;
    c.out += "retry: 3000\n\n";
    c.lastKeepalive = NowSec();
    // Last-Event-ID resume (header, or ?last_event_id= for clients that cannot set headers).
    std::string lastId = req.Header("last-event-id");
    if (lastId.empty()) lastId = req.QueryParam("last_event_id");
    uint64_t seq = 0;
    bool resumed = false;
    if (!lastId.empty() && lastId.find_first_not_of("0123456789") == std::string::npos && lastId.size() < 20) {
      const uint64_t rev = std::strtoull(lastId.c_str(), nullptr, 10);
      if (hub_->SeqForRev(rev, &seq)) {
        std::string frames;
        uint64_t last = 0;
        if (hub_->FramesAfter(seq, &frames, &last)) {
          c.out += frames;
          c.seq = last;
          resumed = true;
        }
      }
    }
    if (!resumed) {
      c.out += hub_->SnapshotFrame();
      c.seq = hub_->LastSeq();
    }
  };

  auto closeConn = [&](Conn& c) {
    if (c.fd >= 0) {
      close(c.fd);
      c.fd = -1;
    }
    if (c.st == St::Streaming) {
      im.streams--;
      counters_.streamsActive.store(static_cast<uint64_t>(std::max(0, im.streams)));
    }
    c.dead = true;
  };

  // Writes as much of c.out as the socket takes. Returns false on a hard error.
  auto flush = [&](Conn& c) -> bool {
    while (c.outOff < c.out.size()) {
      const ssize_t n = send(c.fd, c.out.data() + c.outOff, c.out.size() - c.outOff, MSG_NOSIGNAL);
      if (n > 0) {
        c.outOff += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    if (c.outOff >= c.out.size()) {
      c.out.clear();
      c.outOff = 0;
    } else if (c.outOff > 64 * 1024) {
      c.out.erase(0, c.outOff);
      c.outOff = 0;
    }
    return true;
  };

  std::vector<pollfd> pfds;
  while (!stop_.load(std::memory_order_relaxed)) {
    // 1) New data from the game thread -> stream events.
    const bool changed = hub_->Process();
    const double now = NowSec();
    for (auto& c : im.conns) {
      if (c.dead || c.st != St::Streaming) continue;
      if (changed && hub_->LastSeq() > c.seq) {
        std::string frames;
        uint64_t last = 0;
        if (hub_->FramesAfter(c.seq, &frames, &last)) {
          c.out += frames;
        } else {
          c.out += hub_->SnapshotFrame();  // fell out of the ring: resync
        }
        c.seq = hub_->LastSeq();
      }
      if ((now - c.lastKeepalive) * 1000.0 >= cfg_.keepaliveMs) {
        c.out += SseComment("keepalive");
        c.lastKeepalive = now;
      }
      if (c.out.size() - c.outOff > cfg_.streamMaxBuffered) {
        counters_.streamsDropped.fetch_add(1, std::memory_order_relaxed);
        log("status: dropping a /stream client from " + c.ip + " (write buffer over limit)");
        closeConn(c);
      }
    }

    // 2) Timeouts.
    for (auto& c : im.conns) {
      if (c.dead) continue;
      if ((c.st == St::Reading || c.st == St::Writing) && now > c.deadline) closeConn(c);
    }
    im.conns.erase(std::remove_if(im.conns.begin(), im.conns.end(), [](const Conn& c) { return c.dead; }),
                   im.conns.end());
    counters_.connectionsActive.store(im.conns.size(), std::memory_order_relaxed);

    // 3) poll.
    pfds.clear();
    pfds.push_back({listenFd_, POLLIN, 0});
    pfds.push_back({wakeFds_[0], POLLIN, 0});
    for (auto& c : im.conns) {
      short ev = POLLIN;
      if (c.outOff < c.out.size()) ev |= POLLOUT;
      pfds.push_back({c.fd, ev, 0});
    }
    const int rc = poll(pfds.data(), pfds.size(), cfg_.pollMs);
    if (rc < 0) {
      if (errno == EINTR) continue;
      log(std::string("status: poll failed: ") + std::strerror(errno));
      break;
    }
    const auto loopStart = Clock::now();
    if (pfds[1].revents & POLLIN) {
      char buf[64];
      while (read(wakeFds_[0], buf, sizeof(buf)) > 0) {
      }
    }

    // Existing connections (indices shift by 2).
    const size_t n = im.conns.size();
    for (size_t i = 0; i < n; ++i) {
      Conn& c = im.conns[i];
      const short re = pfds[i + 2].revents;
      if (c.dead || !re) continue;
      if (re & (POLLERR | POLLNVAL)) {
        closeConn(c);
        continue;
      }
      if (re & (POLLIN | POLLHUP)) {
        char buf[4096];
        bool eof = false;
        for (;;) {
          const ssize_t r = recv(c.fd, buf, sizeof(buf), 0);
          if (r > 0) {
            if (c.st == St::Reading) {
              c.in.append(buf, static_cast<size_t>(r));
              if (c.in.size() > cfg_.maxHeaderBytes + 4096) break;
            }
            // Writing / Streaming: request bodies or pipelined requests are ignored.
            continue;
          }
          if (r == 0) {
            eof = true;
          } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            closeConn(c);
          }
          break;
        }
        if (c.dead) continue;
        if (c.st == St::Reading) {
          HttpRequest req;
          const ParseStatus ps = ParseRequest(c.in, cfg_.maxHeaderBytes, &req);
          if (ps == ParseStatus::Ok) {
            handle(c, req);
          } else if (ps == ParseStatus::Bad) {
            respond(c, 400, "application/json", jsonErr("bad request"), false);
          } else if (ps == ParseStatus::TooLarge) {
            respond(c, 431, "application/json", jsonErr("request headers too large"), false);
          }
          if (c.st != St::Reading) {
            c.in.clear();
            c.in.shrink_to_fit();
          }
        }
        // A client that half-closed after a full request still gets its (non-stream) answer.
        if (eof && c.st != St::Writing) {
          closeConn(c);
          continue;
        }
      }
      if (!c.dead && c.outOff < c.out.size()) {
        if (!flush(c)) {
          closeConn(c);
          continue;
        }
      }
      if (!c.dead && c.st == St::Writing && c.out.empty()) {
        shutdown(c.fd, SHUT_WR);
        closeConn(c);
      }
    }
    // Flush streams that got new frames above even if poll reported nothing for them.
    for (auto& c : im.conns) {
      if (c.dead || c.st != St::Streaming || c.outOff >= c.out.size()) continue;
      if (!flush(c)) closeConn(c);
    }

    // New connections.
    if (pfds[0].revents & POLLIN) {
      for (int k = 0; k < 64; ++k) {
        sockaddr_storage ss{};
        socklen_t len = sizeof(ss);
        const int fd = accept(listenFd_, reinterpret_cast<sockaddr*>(&ss), &len);
        if (fd < 0) break;
        SetNonBlockingCloexec(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        int live = 0;
        for (const auto& c : im.conns) live += c.dead ? 0 : 1;
        if (live >= cfg_.maxConnections) {
          counters_.rejectedConnections.fetch_add(1, std::memory_order_relaxed);
          const std::string r = BuildResponse(503, "application/json", "{\"error\":\"too many connections\"}\n", false,
                                              {{"Retry-After", "1"}});
          char drain[2048];
          while (recv(fd, drain, sizeof(drain), 0) > 0) {
          }
          (void)!send(fd, r.data(), r.size(), MSG_NOSIGNAL);
          shutdown(fd, SHUT_WR);
          close(fd);
          continue;
        }
        Conn c;
        c.fd = fd;
        c.ip = PeerIp(ss);
        if (c.ip.rfind("::ffff:", 0) == 0 && c.ip.find('.') != std::string::npos) c.ip = c.ip.substr(7);
        c.loopback = IsLoopbackAddress(c.ip);
        c.st = St::Reading;
        c.deadline = NowSec() + cfg_.readTimeoutMs / 1000.0;
        im.conns.push_back(std::move(c));
      }
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - loopStart).count();
    if (static_cast<uint64_t>(us) > counters_.loopIterationsMaxUs.load(std::memory_order_relaxed)) {
      counters_.loopIterationsMaxUs.store(static_cast<uint64_t>(us), std::memory_order_relaxed);
    }
  }
  for (auto& c : im.conns) closeConn(c);
  im.conns.clear();
}

}  // namespace readyup::status
