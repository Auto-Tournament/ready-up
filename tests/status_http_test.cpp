// Offline tests for the local status endpoint (docs/FLEET.md §17): the HTTP request parser,
// token / rate-limit helpers, JSON merge patches, SSE framing, the snapshot Hub, and the real
// server (status_server.cpp) on an ephemeral loopback port. No CS2 server needed:
//   cmake --build build && (cd build && ctest --output-on-failure)

#include "readyup/minijson.h"
#include "readyup/status_http.h"
#include "readyup/status_server.h"
#include "readyup/status_snapshot.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace readyup::status;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static void CheckStr(const std::string& a, const std::string& b, const char* expr, int line) {
  if (a == b) return;
  std::fprintf(stderr, "%s:%d: CHECK_STR failed: %s:\n  got:  \"%s\"\n  want: \"%s\"\n", __FILE__, line, expr,
               a.c_str(), b.c_str());
  ++g_failures;
}
#define CHECK_STR(a, b) CheckStr((a), (b), #a, __LINE__)

static bool Contains(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

static bool ParsesAsJson(const std::string& s) {
  readyup::minijson::ParseError err;
  auto v = readyup::minijson::Parse(s, &err);
  if (!v) std::fprintf(stderr, "JSON parse error at %zu: %s in %s\n", err.offset, err.msg.c_str(), s.c_str());
  return v.has_value();
}

// ---------------------------------------------------------------------------- parser

static void TestParser() {
  HttpRequest r;
  CHECK(ParseRequest("GET /status HTTP/1.1\r\nHost: x\r\n\r\n", 8192, &r) == ParseStatus::Ok);
  CHECK_STR(r.method, "GET");
  CHECK_STR(r.path, "/status");
  CHECK_STR(r.Header("host"), "x");

  CHECK(ParseRequest("GET /status HTTP/1.1\r\nHost: x\r\n", 8192, &r) == ParseStatus::Incomplete);
  CHECK(ParseRequest("GET /sta", 8192, &r) == ParseStatus::Incomplete);
  // HTTP/1.1 needs Host; 1.0 does not.
  CHECK(ParseRequest("GET / HTTP/1.1\r\n\r\n", 8192, &r) == ParseStatus::Bad);
  CHECK(ParseRequest("GET / HTTP/1.0\r\n\r\n", 8192, &r) == ParseStatus::Ok);
  // Bare LF line endings, leading empty line, header case + whitespace.
  CHECK(ParseRequest("\r\nGET /health/ HTTP/1.1\nHOST:  h \nX-ReadyUp-Token:\tabc\n\n", 8192, &r) == ParseStatus::Ok);
  CHECK_STR(r.path, "/health");
  CHECK_STR(r.Header("host"), "h");
  CHECK_STR(r.Header("x-readyup-token"), "abc");
  // Query + percent decoding.
  CHECK(ParseRequest("GET /selftest?run=1&token=a%2Bb+c&x HTTP/1.1\r\nHost: h\r\n\r\n", 8192, &r) == ParseStatus::Ok);
  CHECK_STR(r.path, "/selftest");
  CHECK_STR(r.QueryParam("run"), "1");
  CHECK_STR(r.QueryParam("token"), "a+b c");
  bool present = false;
  CHECK_STR(r.QueryParam("x", &present), "");
  CHECK(present);
  r.QueryParam("missing", &present);
  CHECK(!present);
  CHECK(ParseRequest("GET /st%61tus HTTP/1.1\r\nHost: h\r\n\r\n", 8192, &r) == ParseStatus::Ok);
  CHECK_STR(r.path, "/status");
  // Absolute form.
  CHECK(ParseRequest("GET http://127.0.0.1:27105/status?a=1 HTTP/1.1\r\nHost: h\r\n\r\n", 8192, &r) == ParseStatus::Ok);
  CHECK_STR(r.path, "/status");
  CHECK_STR(r.query, "a=1");

  // Malformed.
  CHECK(ParseRequest("GET /status\r\nHost: h\r\n\r\n", 8192, &r) == ParseStatus::Bad);
  CHECK(ParseRequest("GET  /status HTTP/1.1\r\nHost: h\r\n\r\n", 8192, &r) == ParseStatus::Bad);
  CHECK(ParseRequest("GET /status HTTP/2\r\nHost: h\r\n\r\n", 8192, &r) == ParseStatus::Bad);
  CHECK(ParseRequest("G(T /status HTTP/1.1\r\nHost: h\r\n\r\n", 8192, &r) == ParseStatus::Bad);
  CHECK(ParseRequest("GET status HTTP/1.1\r\nHost: h\r\n\r\n", 8192, &r) == ParseStatus::Bad);
  CHECK(ParseRequest("GET /status HTTP/1.1\r\nHost h\r\n\r\n", 8192, &r) == ParseStatus::Bad);
  CHECK(ParseRequest("GET /status HTTP/1.1\r\nHost: h\r\n folded\r\n\r\n", 8192, &r) == ParseStatus::Bad);
  CHECK(ParseRequest("GET /a\x01 HTTP/1.1\r\nHost: h\r\n\r\n", 8192, &r) == ParseStatus::Bad);
  // Bodies are refused (GET/HEAD only, no body support).
  CHECK(ParseRequest("POST /status HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello", 8192, &r) == ParseStatus::Bad);
  CHECK(ParseRequest("GET /status HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n", 8192, &r) == ParseStatus::Bad);
  CHECK(ParseRequest("GET /status HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n", 8192, &r) == ParseStatus::Ok);
  // Methods other than GET/HEAD parse fine (the router answers 405).
  CHECK(ParseRequest("DELETE /status HTTP/1.1\r\nHost: h\r\n\r\n", 8192, &r) == ParseStatus::Ok);

  // Size limit: incomplete and over the limit, or complete but too long.
  std::string big = "GET / HTTP/1.1\r\nHost: h\r\nX: " + std::string(9000, 'a');
  CHECK(ParseRequest(big, 8192, &r) == ParseStatus::TooLarge);
  CHECK(ParseRequest(big + "\r\n\r\n", 8192, &r) == ParseStatus::TooLarge);
  std::string many = "GET / HTTP/1.1\r\nHost: h\r\n";
  for (int i = 0; i < 120; ++i) many += "X" + std::to_string(i) + ": y\r\n";
  CHECK(ParseRequest(many + "\r\n", 1 << 20, &r) == ParseStatus::TooLarge);
}

static void TestHelpers() {
  HttpRequest r;
  ParseRequest("GET /status HTTP/1.1\r\nHost: h\r\nAuthorization: Bearer  rst_abc \r\n\r\n", 8192, &r);
  CHECK_STR(ExtractToken(r), "rst_abc");
  ParseRequest("GET /status HTTP/1.1\r\nHost: h\r\nX-ReadyUp-Token: t2\r\n\r\n", 8192, &r);
  CHECK_STR(ExtractToken(r), "t2");
  ParseRequest("GET /status?token=t3 HTTP/1.1\r\nHost: h\r\n\r\n", 8192, &r);
  CHECK_STR(ExtractToken(r), "t3");
  ParseRequest("GET /status HTTP/1.1\r\nHost: h\r\nAuthorization: Basic Zm9v\r\n\r\n", 8192, &r);
  CHECK_STR(ExtractToken(r), "");

  CHECK(TokenEquals("rst_abc", "rst_abc"));
  CHECK(!TokenEquals("rst_abc", "rst_abd"));
  CHECK(!TokenEquals("rst_abc", "rst_ab"));
  CHECK(!TokenEquals("", ""));

  RateLimiter rl(5, 20, 4);
  int ok = 0;
  for (int i = 0; i < 25; ++i) ok += rl.Allow("1.2.3.4", 100.0) ? 1 : 0;
  CHECK(ok == 20);
  CHECK(!rl.Allow("1.2.3.4", 100.1));  // 0.5 token
  CHECK(rl.Allow("1.2.3.4", 100.2));   // 1 token after 0.2 s at 5/s
  CHECK(rl.Allow("5.6.7.8", 100.2));   // separate bucket
  // Key table bound: full buckets are pruned, a flood of fresh keys is refused.
  CHECK(rl.Allow("k3", 100.2));
  CHECK(rl.Allow("k4", 100.2));
  CHECK(!rl.Allow("k5", 100.2));  // 4 keys, none idle long enough to prune
  CHECK(rl.Allow("k5", 200.0));   // all idle -> pruned
  CHECK(rl.Size() <= 4);

#define LIT(s) std::string(s, sizeof(s) - 1)
  CHECK(GamePortFromCmdline(LIT("./cs2\0-dedicated\0-port\00027055\0+map\0de_dust2")) == 27055);
  CHECK(GamePortFromCmdline(LIT("cs2\0+hostport\00027025\0")) == 27025);
  CHECK(GamePortFromCmdline(LIT("cs2\0-dedicated")) == 27015);
#undef LIT
  CHECK(GamePortFromCmdline("") == 27015);

  CHECK(IsLoopbackAddress("127.0.0.1"));
  CHECK(IsLoopbackAddress("127.1.2.3"));
  CHECK(IsLoopbackAddress("::1"));
  CHECK(!IsLoopbackAddress("0.0.0.0"));
  CHECK(!IsLoopbackAddress("10.0.0.1"));

  const std::string resp = BuildResponse(405, "application/json", "{}", true, {{"Allow", "GET, HEAD"}});
  CHECK(Contains(resp, "HTTP/1.1 405 Method Not Allowed\r\n"));
  CHECK(Contains(resp, "Content-Length: 2\r\n"));
  CHECK(Contains(resp, "Allow: GET, HEAD\r\n"));
  CHECK(resp.size() >= 4 && resp.substr(resp.size() - 4) == "\r\n\r\n");  // HEAD: no body
}

// ---------------------------------------------------------------------------- json / sse

static void TestJson() {
  Json j = Json::Object();
  j["s"] = "a\"b\\c\n\x01";
  j["i"] = 42;
  j["b"] = true;
  j["n"] = Json();
  j["d"] = 1.5;
  j["arr"].Push(1);
  j["arr"].Push("x");
  j["o"]["k"] = 7LL;
  CHECK_STR(j.Dump(), "{\"s\":\"a\\\"b\\\\c\\n\\u0001\",\"i\":42,\"b\":true,\"n\":null,\"d\":1.5,\"arr\":[1,\"x\"],\"o\":{\"k\":7}}");
  CHECK(ParsesAsJson(j.Dump()));

  // Equality ignores key order.
  Json a = Json::Object(), b = Json::Object();
  a["x"] = 1;
  a["y"] = 2;
  b["y"] = 2;
  b["x"] = 1;
  CHECK(a == b);

  // Merge diff: changed leaf, removed key, added key, nested object, array replaced whole.
  Json from = Json::Object();
  from["phase"] = "warmup";
  from["round"]["number"] = 0;
  from["teams"]["team1"]["score"] = 0;
  from["teams"]["team1"]["name"] = "A";
  from["teams"]["team2"]["score"] = 0;
  from["gone"] = 1;
  from["list"].Push(1);
  Json to = from;
  to["phase"] = "live";
  to["round"]["number"] = 14;
  to["teams"]["team1"]["score"] = 8;
  to["list"].Push(2);
  to["new"]["x"] = true;
  Json to2 = Json::Object();
  for (const auto& kv : to.Members()) {
    if (kv.first != "gone") to2[kv.first] = kv.second;
  }
  Json patch;
  CHECK(MergeDiff(from, to2, &patch));
  CHECK_STR(patch.Dump(),
            "{\"gone\":null,\"phase\":\"live\",\"round\":{\"number\":14},\"teams\":{\"team1\":{\"score\":8}},"
            "\"list\":[1,2],\"new\":{\"x\":true}}");
  CHECK(MergePatchApply(from, patch) == to2);
  CHECK(!MergeDiff(to2, to2, &patch));
  // null <-> object at the top level (no match loaded / loaded).
  CHECK(MergeDiff(Json(), to2, &patch));
  CHECK(patch == to2);
  CHECK(MergeDiff(to2, Json(), &patch));
  CHECK(patch.IsNull());
  // A member set to null is expressed as deletion.
  Json withNull = to2;
  withNull["phase"] = Json();
  CHECK(MergeDiff(to2, withNull, &patch));
  CHECK_STR(patch.Dump(), "{\"phase\":null}");

  CHECK_STR(SseFrame("patch", "513", "{\"rev\":513}"), "event: patch\nid: 513\ndata: {\"rev\":513}\n\n");
  CHECK_STR(SseFrame("status", "", "a\nb\r\nc"), "event: status\ndata: a\ndata: b\ndata: c\n\n");
  CHECK_STR(SseComment("keepalive"), ": keepalive\n\n");
}

static std::shared_ptr<StatusInputs> Inputs(const std::string& phase, int round, const std::string& mode = "match") {
  auto in = std::make_shared<StatusInputs>();
  in->hostname = "test-host";
  in->game_port = 27055;
  in->versions["core"] = "0.0.0-test";
  in->platform["mode"] = "standalone";
  in->platform["state"] = "standalone";
  in->summary["mode"] = mode;
  in->summary["phase"] = phase;
  in->summary["round"] = round;
  in->summary["players"]["connected"] = 2;
  in->summary["players"]["expected"] = 10;
  in->state["match_id"] = "42";
  in->state["phase"] = phase;
  in->state["round"]["number"] = round;
  in->update_safe = mode != "match";
  in->selftest_report = "selftest: PASS 3/3";
  in->selftest["pass"] = true;
  in->gauges.emplace_back("status_feed_build_us_last", 12);
  return in;
}

static void TestHub() {
  Hub hub(4);
  CHECK(!hub.Process());
  CHECK(hub.StatusBody(0, 0) == "{\"error\":\"starting\"}");
  hub.Submit(Inputs("warmup", 0));
  CHECK(hub.Process());
  CHECK(hub.Rev() == 1);
  CHECK(hub.LastSeq() == 0);
  const std::string body = hub.StatusBody(5, 1790000000000LL);
  CHECK(ParsesAsJson(body));
  CHECK(Contains(body, "\"uptime_s\":5"));
  CHECK(Contains(body, "\"generated_at\":1790000000000"));
  CHECK(Contains(body, "\"update_safe\":false"));
  CHECK(Contains(body, "\"state\":{\"match_id\":\"42\""));
  CHECK(Contains(hub.SnapshotFrame(), "event: snapshot\nid: 1\ndata: {\"rev\":1,"));

  // Same inputs again: nothing to send.
  hub.Submit(Inputs("warmup", 0));
  CHECK(!hub.Process());
  CHECK(hub.Rev() == 1);

  // State + summary change: one patch and one status event.
  hub.Submit(Inputs("live", 1));
  CHECK(hub.Process());
  CHECK(hub.Rev() == 2);
  std::string frames;
  uint64_t last = 0;
  CHECK(hub.FramesAfter(0, &frames, &last));
  CHECK(last == 2);
  CHECK(Contains(frames, "event: patch\nid: 2\ndata: {\"rev\":2,\"patch\":{\"phase\":\"live\",\"round\":{\"number\":1}}}\n\n"));
  CHECK(Contains(frames, "event: status\ndata: {\"summary\":{"));

  // Only a non-state field changes: status event, rev unchanged.
  hub.Submit(Inputs("live", 1, "scrim"));
  CHECK(hub.Process());
  CHECK(hub.Rev() == 2);
  frames.clear();
  CHECK(hub.FramesAfter(2, &frames, &last));
  CHECK(Contains(frames, "\"update_safe\":true"));
  CHECK(!Contains(frames, "event: patch"));

  // Resume by rev.
  uint64_t seq = 0;
  CHECK(hub.SeqForRev(2, &seq));
  CHECK(seq == 1);
  CHECK(!hub.SeqForRev(99, &seq));

  // Ring overflow: the oldest events drop out; FramesAfter(0) then asks for a snapshot.
  for (int i = 2; i < 12; ++i) {
    hub.Submit(Inputs("live", i, i % 2 ? "scrim" : "match"));
    hub.Process();
  }
  CHECK(hub.Rev() == 12);
  CHECK(hub.RingSize() <= 8);
  frames.clear();
  CHECK(!hub.FramesAfter(0, &frames, &last));
  CHECK(!hub.SeqForRev(2, &seq));
  CHECK(hub.SeqForRev(12, &seq));
}

// ---------------------------------------------------------------------------- server

static int Connect(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// Reads until `until` shows up in what was read (or EOF / timeout). Returns everything read.
static std::string ReadUntil(int fd, const std::string& until, int timeoutMs) {
  std::string out;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!until.empty() && Contains(out, until)) break;
    pollfd p{fd, POLLIN, 0};
    const int left = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    if (poll(&p, 1, std::max(1, left)) <= 0) break;
    char buf[8192];
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  return out;
}

static std::string Http(int port, const std::string& raw, int timeoutMs = 3000) {
  const int fd = Connect(port);
  if (fd < 0) return "CONNECT FAILED";
  (void)!send(fd, raw.data(), raw.size(), MSG_NOSIGNAL);
  std::string r = ReadUntil(fd, "", timeoutMs);
  close(fd);
  return r;
}

static std::string Get(int port, const std::string& path, const std::string& extra = "") {
  return Http(port, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n" + extra + "\r\n");
}

static std::string Body(const std::string& resp) {
  const size_t p = resp.find("\r\n\r\n");
  return p == std::string::npos ? std::string() : resp.substr(p + 4);
}

static ServerConfig TestCfg() {
  ServerConfig c;
  c.bind = "127.0.0.1";
  c.port = 0;
  c.token = "rst_0123456789abcdef0123";
  c.pollMs = 20;
  c.ratePerSec = 1000;
  c.rateBurst = 1000;
  return c;
}

static void TestServerBasics() {
  auto hub = std::make_shared<Hub>();
  hub->Submit(Inputs("warmup", 0));
  std::atomic<int> selftestRequests{0};
  ServerConfig cfg = TestCfg();
  cfg.metrics = true;
  cfg.requestSelftest = [&] { selftestRequests++; };
  StatusServer srv(cfg, hub);
  std::string err;
  CHECK(srv.Start(&err));
  const int port = srv.BoundPort();
  CHECK(port > 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));

  std::string r = Get(port, "/health");
  CHECK(Contains(r, "HTTP/1.1 200 OK"));
  CHECK(Contains(Body(r), "{\"ok\":true,\"uptime_s\":"));
  CHECK(Contains(r, "Connection: close"));

  r = Get(port, "/status");
  CHECK(Contains(r, "HTTP/1.1 200 OK"));
  CHECK(Contains(r, "Content-Type: application/json"));
  CHECK(ParsesAsJson(Body(r)));
  for (const char* k : {"\"hostname\":\"test-host\"", "\"game_port\":27055", "\"uptime_s\":", "\"generated_at\":",
                        "\"versions\":", "\"selftest\":", "\"platform\":", "\"update_safe\":", "\"summary\":",
                        "\"state\":"}) {
    CHECK(Contains(r, k));
  }
  // Content-Length matches the body.
  {
    const size_t p = r.find("Content-Length: ");
    CHECK(p != std::string::npos);
    CHECK(std::stoul(r.substr(p + 16)) == Body(r).size());
  }

  r = Http(port, "HEAD /status HTTP/1.1\r\nHost: h\r\n\r\n");
  CHECK(Contains(r, "HTTP/1.1 200 OK"));
  CHECK(Body(r).empty());

  r = Http(port, "POST /status HTTP/1.1\r\nHost: h\r\n\r\n");
  CHECK(Contains(r, "HTTP/1.1 405"));
  CHECK(Contains(r, "Allow: GET, HEAD"));
  r = Http(port, "PUT /status HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc");
  CHECK(Contains(r, "HTTP/1.1 400"));  // bodies are refused before routing
  CHECK(Contains(Get(port, "/nope"), "HTTP/1.1 404"));
  CHECK(Contains(Http(port, "garbage\r\n\r\n"), "HTTP/1.1 400"));
  CHECK(Contains(Http(port, "GET / HTTP/1.1\r\nHost: h\r\nX: " + std::string(10000, 'a') + "\r\n\r\n"), "HTTP/1.1 431"));
  CHECK(Contains(Get(port, "/"), "\"/stream\""));

  r = Get(port, "/metrics");
  CHECK(Contains(r, "text/plain; version=0.0.4"));
  CHECK(Contains(r, "\nreadyup_up 1\n"));
  CHECK(Contains(r, "readyup_update_safe 0"));
  CHECK(Contains(r, "readyup_players_expected 10"));
  CHECK(Contains(r, "readyup_status_feed_build_us_last 12"));
  CHECK(Contains(r, "readyup_http_requests_total{code=\"200\"}"));

  r = Get(port, "/selftest");
  CHECK(Contains(r, "text/plain"));
  CHECK(Contains(Body(r), "selftest: PASS 3/3"));
  r = Get(port, "/selftest?run=1");
  CHECK(Contains(r, "HTTP/1.1 202"));
  CHECK(selftestRequests.load() == 1);
  r = Get(port, "/selftest?run=1");
  CHECK(Contains(r, "HTTP/1.1 429"));  // min interval
  CHECK(selftestRequests.load() == 1);

  // A client that half-closes right after its request still gets the answer.
  {
    const int fd = Connect(port);
    const std::string req = "GET /health HTTP/1.1\r\nHost: h\r\n\r\n";
    (void)!send(fd, req.data(), req.size(), MSG_NOSIGNAL);
    shutdown(fd, SHUT_WR);
    CHECK(Contains(ReadUntil(fd, "", 2000), "HTTP/1.1 200 OK"));
    close(fd);
  }
  // Request split across packets.
  {
    const int fd = Connect(port);
    (void)!send(fd, "GET /hea", 8, MSG_NOSIGNAL);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    (void)!send(fd, "lth HTTP/1.1\r\nHost: h\r\n\r\n", 25, MSG_NOSIGNAL);
    CHECK(Contains(ReadUntil(fd, "", 2000), "HTTP/1.1 200 OK"));
    close(fd);
  }
  srv.Stop();
  CHECK(!srv.Running());
}

static void TestServerAuthAndLimits() {
  auto hub = std::make_shared<Hub>();
  hub->Submit(Inputs("warmup", 0));
  ServerConfig cfg = TestCfg();
  cfg.trustLoopback = false;  // behave like a remote client
  cfg.maxConnections = 3;
  cfg.readTimeoutMs = 300;
  cfg.ratePerSec = 1;
  cfg.rateBurst = 12;
  StatusServer srv(cfg, hub);
  std::string err;
  CHECK(srv.Start(&err));
  const int port = srv.BoundPort();

  CHECK(Contains(Get(port, "/health"), "HTTP/1.1 200"));  // never needs the token
  std::string r = Get(port, "/status");
  CHECK(Contains(r, "HTTP/1.1 401"));
  CHECK(Contains(r, "WWW-Authenticate: Bearer"));
  CHECK(Contains(Get(port, "/status", "Authorization: Bearer wrong\r\n"), "HTTP/1.1 401"));
  CHECK(Contains(Get(port, "/status", "Authorization: Bearer rst_0123456789abcdef0123\r\n"), "HTTP/1.1 200"));
  CHECK(Contains(Get(port, "/status?token=rst_0123456789abcdef0123"), "HTTP/1.1 200"));
  CHECK(Contains(Get(port, "/stream?token=nope"), "HTTP/1.1 401"));
  CHECK(Contains(Get(port, "/metrics", "X-ReadyUp-Token: rst_0123456789abcdef0123\r\n"), "HTTP/1.1 404"));  // disabled

  // Connection cap: three idle connections, the fourth is refused with 503.
  int idle[3];
  for (int& fd : idle) fd = Connect(port);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  r = Get(port, "/health");
  CHECK(!Contains(r, "HTTP/1.1 200"));  // 503 (or a reset if the request raced the close)
  CHECK(srv.counters().rejectedConnections.load() >= 1);
  // Read timeout: the idle ones are closed after ~300 ms.
  for (int fd : idle) {
    const std::string got = ReadUntil(fd, "", 2000);
    CHECK(got.empty());  // closed without a response
    close(fd);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(60));

  // Rate limit: burst 12 at 1/s. Some of the burst is already used above.
  int limited = 0;
  for (int i = 0; i < 15; ++i) {
    if (Contains(Get(port, "/health"), "HTTP/1.1 429")) ++limited;
  }
  CHECK(limited > 0);
  CHECK(srv.counters().rateLimited.load() > 0);
  srv.Stop();

  // Non-loopback bind needs a real token.
  ServerConfig bad = TestCfg();
  bad.bind = "0.0.0.0";
  bad.token = "short";
  StatusServer s2(bad, hub);
  CHECK(!s2.Start(&err));
  CHECK(Contains(err, "without a status_http_token"));
  ServerConfig badAddr = TestCfg();
  badAddr.bind = "localhost";  // numeric only
  StatusServer s3(badAddr, hub);
  CHECK(!s3.Start(&err));
}

static void TestServerStream() {
  auto hub = std::make_shared<Hub>(256);
  hub->Submit(Inputs("warmup", 0));
  ServerConfig cfg = TestCfg();
  cfg.keepaliveMs = 300;
  cfg.maxStreams = 2;
  StatusServer srv(cfg, hub);
  std::string err;
  CHECK(srv.Start(&err));
  const int port = srv.BoundPort();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));

  const int s1 = Connect(port);
  const std::string req = "GET /stream HTTP/1.1\r\nHost: h\r\nAccept: text/event-stream\r\n\r\n";
  (void)!send(s1, req.data(), req.size(), MSG_NOSIGNAL);
  std::string got = ReadUntil(s1, "\n\n", 2000);
  got += ReadUntil(s1, "event: snapshot", 2000);
  got += ReadUntil(s1, "\n\n", 500);
  CHECK(Contains(got, "HTTP/1.1 200 OK"));
  CHECK(Contains(got, "Content-Type: text/event-stream"));
  CHECK(Contains(got, "retry: 3000\n\n"));
  CHECK(Contains(got, "event: snapshot\nid: 1\ndata: {\"rev\":1,"));

  // State change -> patch (id 2); summary-only change -> status.
  hub->Submit(Inputs("live", 1));
  got = ReadUntil(s1, "event: status", 2000);
  got += ReadUntil(s1, "\n\n", 500);
  CHECK(Contains(got, "event: patch\nid: 2\ndata: {\"rev\":2,\"patch\":{\"phase\":\"live\",\"round\":{\"number\":1}}}\n\n"));
  CHECK(Contains(got, "event: status\ndata: {\"summary\":"));

  // Keepalive comment.
  got = ReadUntil(s1, ": keepalive\n\n", 2000);
  CHECK(Contains(got, ": keepalive\n\n"));

  hub->Submit(Inputs("live", 2));
  CHECK(Contains(ReadUntil(s1, "id: 3", 2000), "id: 3"));

  // Resume from rev 2: gets patch 3 without a snapshot.
  const int s2 = Connect(port);
  const std::string resume = "GET /stream HTTP/1.1\r\nHost: h\r\nLast-Event-ID: 2\r\n\r\n";
  (void)!send(s2, resume.data(), resume.size(), MSG_NOSIGNAL);
  got = ReadUntil(s2, "id: 3\n", 2000);
  got += ReadUntil(s2, "\n\n", 300);
  CHECK(Contains(got, "event: patch\nid: 3\n"));
  CHECK(!Contains(got, "event: snapshot"));

  // Stream cap (2): the third one gets 503.
  std::string third = Get(port, "/stream");
  CHECK(Contains(third, "HTTP/1.1 503"));
  CHECK(srv.counters().streamsActive.load() == 2);

  // Unknown Last-Event-ID -> fresh snapshot.
  close(s2);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const int s3 = Connect(port);
  const std::string stale = "GET /stream HTTP/1.1\r\nHost: h\r\nLast-Event-ID: 99999\r\n\r\n";
  (void)!send(s3, stale.data(), stale.size(), MSG_NOSIGNAL);
  got = ReadUntil(s3, "event: snapshot\nid: 3\n", 2000);
  CHECK(Contains(got, "event: snapshot\nid: 3\n"));

  close(s1);
  close(s3);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(srv.counters().streamsActive.load() == 0);
  srv.Stop();
}

// A stream client that never reads is dropped once its buffer passes the cap, and the
// server keeps answering everyone else meanwhile.
static void TestSlowStreamDropped() {
  auto hub = std::make_shared<Hub>(256);
  hub->Submit(Inputs("warmup", 0));
  ServerConfig cfg = TestCfg();
  cfg.streamMaxBuffered = 64 * 1024;
  StatusServer srv(cfg, hub);
  std::string err;
  CHECK(srv.Start(&err));
  const int port = srv.BoundPort();
  const int s = Connect(port);
  int small = 4096;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
  const std::string req = "GET /stream HTTP/1.1\r\nHost: h\r\n\r\n";
  (void)!send(s, req.data(), req.size(), MSG_NOSIGNAL);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 3000 && srv.counters().streamsDropped.load() == 0; ++i) {
    auto in = Inputs("live", i);
    in->state["filler"] = std::string(32 * 1024, static_cast<char>('a' + i % 26));
    hub->Submit(in);
    if (i % 50 == 0) {
      CHECK(Contains(Get(port, "/health"), "HTTP/1.1 200"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(srv.counters().streamsDropped.load() == 1);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20));
  close(s);
  srv.Stop();
}

int main() {
  TestParser();
  TestHelpers();
  TestJson();
  TestHub();
  TestServerBasics();
  TestServerAuthAndLimits();
  TestServerStream();
  TestSlowStreamDropped();
  if (g_failures) {
    std::fprintf(stderr, "status_http_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("status_http_test: all passed\n");
  return 0;
}
