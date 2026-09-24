// Integration test: the real fleet::Client (libcurl WebSocket, spool, credentials) against the
// mock platform in tests/mock_platform.cpp over 127.0.0.1. Covers enroll, hello/welcome, ping/pong,
// seq/ack both ways, unknown types, reconnect with backoff, resume with replay, reset + snapshot,
// resume after a process restart, heartbeat timeout, 4401 and a refused one-time code.
//   build/fleet_integration_test
#include "fleet_client.h"
#include "mock_platform.h"
#include "test_util.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

using namespace fleet;

namespace {

std::string TempDir() {
  char tmpl[] = "/tmp/fleet_it_XXXXXX";
  const char* d = mkdtemp(tmpl);
  return d ? d : "/tmp";
}

ClientConfig BaseConfig(const mock::Platform& p, const std::string& dir) {
  ClientConfig c;
  c.url = p.BaseUrl();
  c.insecureDev = true;
  c.dataDir = dir;
  c.backoff.baseMs = 100;
  c.backoff.capMs = 400;
  c.backoff.resetAfterMs = 60000;
  c.connectTimeoutMs = 3000;
  c.helloTimeoutMs = 3000;
  c.rngSeed = 7;
  c.log = [](int level, const std::string& msg) { std::printf("    [client %d] %s\n", level, msg.c_str()); };
  return c;
}

HelloInfo Hello() {
  HelloInfo h;
  h.coreVersion = "0.9.0 (test)";
  h.plugins = {{"fleet", "0.9.0"}};
  h.cs2Build = 14032;
  h.cs2Patch = "1.40.3.2";
  h.hostname = "it-host";
  h.gamePort = 27055;
  h.bootId = NewUlid(NowMs());
  h.capabilities = {"match.v1"};
  h.startedMs = NowMs();
  return h;
}

bool WaitState(Client& c, LinkState s, int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    if (c.Status().state == s) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return c.Status().state == s;
}

std::string StreamOf(const mock::Received& hello) {
  return hello.payload()->Get("stream")->Get("id")->AsStr();
}

}  // namespace

// Enroll with a one-time code, then hello/welcome/ping/pong, reliable both ways.
TEST(TestEnrollConnectPingAck) {
  mock::Platform p;
  CHECK(p.Start());
  p.heartbeatIntervalMs = 300;
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollCode = "RUE-7F3K-9QX2-LM4D";
  Client c(cfg);
  c.SetHelloInfo(Hello());
  c.SetHandledTypes({"test.echo"}, false);
  std::string err;
  CHECK(c.Start(&err));
  CHECK(WaitState(c, LinkState::Online, 5000));
  CHECK_EQ(p.enrollments(), 1);
  // Enrollment body: code + install_id + versions; credentials.json 0600 with the token.
  json::Value body;
  CHECK(json::Parse(p.lastEnrollBody(), &body));
  CHECK_EQ(body.Get("code")->AsStr(), std::string("RUE-7F3K-9QX2-LM4D"));
  CHECK_EQ(body.Get("install_id")->AsStr(), c.installId());
  CHECK_EQ(body.Get("cs2_build")->AsInt(), int64_t(14032));
  struct stat st {};
  CHECK(stat((dir + "/credentials.json").c_str(), &st) == 0);
  CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0600);
  CHECK_EQ(p.lastAuthHeader(), "Bearer " + p.token);
  // hello
  CHECK(p.WaitFor([&] { return !p.MessagesOfType("hello").empty(); }, 2000));
  const auto hello = p.MessagesOfType("hello").at(0);
  const json::Value* hp = hello.payload();
  CHECK_EQ(hp->Get("server_id")->AsStr(), p.serverId);
  CHECK_EQ(hp->Get("install_id")->AsStr(), c.installId());
  CHECK_EQ(hp->Get("tenant_id")->AsStr(), std::string("default"));
  CHECK_EQ(hp->Get("protocol")->Get("max")->AsInt(), int64_t(1));
  CHECK_EQ(hp->Get("versions")->Get("cs2_build")->AsInt(), int64_t(14032));
  CHECK_EQ(hp->Get("versions")->Get("plugins")->Get("fleet")->AsStr(), std::string("0.9.0"));
  CHECK_EQ(hp->Get("capabilities")->a.at(0).AsStr(), std::string("match.v1"));
  CHECK_EQ(hp->Get("host")->Get("game_port")->AsInt(), int64_t(27055));
  CHECK_EQ(hp->Get("stream")->Get("last_tx_seq")->AsInt(), int64_t(0));
  CHECK(hp->Get("state")->IsNull());
  CHECK_EQ(hp->Get("availability")->AsStr(), std::string("available"));
  CHECK(IsUlid(hello.env.Get("id")->AsStr()));
  CHECK(hello.env.Get("seq") == nullptr);  // hello is ephemeral
  CHECK_EQ(c.Status().sessionId, std::string("sess_1"));
  // ping with health every heartbeat interval (300 ms here)
  CHECK(p.WaitFor([&] { return p.MessagesOfType("ping").size() >= 2; }, 3000));
  const auto ping = p.MessagesOfType("ping").at(0);
  CHECK(ping.payload()->Get("health") && ping.payload()->Get("health")->Get("spool_msgs"));
  CHECK(p.WaitFor([&] { return c.Status().rttMs >= 0; }, 2000));  // our ping got the mock's pong
  // platform ping -> our pong carries t and ref
  CHECK(p.SendToServer("ping", R"({"t":424242})", false));
  CHECK(p.WaitFor([&] { return !p.MessagesOfType("pong").empty(); }, 2000));
  CHECK_EQ(p.MessagesOfType("pong").at(0).payload()->Get("t")->AsInt(), int64_t(424242));
  // server -> platform reliable: seq 1, acked, spool drained
  CHECK(c.Send("event.phase", R"({"from":"warmup","to":"live"})", 3, true, &err));
  CHECK(p.WaitFor([&] { return !p.MessagesOfType("event.phase").empty(); }, 2000));
  const auto ev = p.MessagesOfType("event.phase").at(0);
  CHECK_EQ(ev.seq(), int64_t(1));
  CHECK_EQ(ev.env.Get("epoch")->AsInt(), int64_t(3));
  CHECK(p.WaitFor([&] { return c.Status().ackedSeq == 1 && c.Status().spoolMsgs == 0; }, 2000));
  CHECK(!c.Send("hello", "{}", 0, true, &err));        // reserved type
  CHECK(!c.Send("event.x", "[1]", 0, true, &err));     // payload must be an object
  // platform -> server reliable, unknown type: error{unknown_type} + ack
  CHECK(p.SendToServer("server.drain", R"({"reason":"x"})", true));
  CHECK(p.WaitFor([&] { return !p.MessagesOfType("error").empty(); }, 2000));
  CHECK_EQ(p.MessagesOfType("error").at(0).payload()->Get("code")->AsStr(), std::string("unknown_type"));
  CHECK(p.WaitFor([&] { return p.lastAckFromServer() >= 1; }, 2000));
  // handled type: delivered to the "game thread", acked only after MarkProcessed
  CHECK(p.SendToServer("test.echo", R"({"n":"\u00e9"})", true));
  std::deque<Inbound> in;
  CHECK(p.WaitFor([&] {
    auto more = c.TakeInbound();
    for (auto& m : more) in.push_back(std::move(m));
    return std::any_of(in.begin(), in.end(), [](const Inbound& i) { return i.env.type == "test.echo"; });
  }, 2000));
  const auto it = std::find_if(in.begin(), in.end(), [](const Inbound& i) { return i.env.type == "test.echo"; });
  CHECK(it != in.end() && it->reliable && it->env.seq == 2);
  CHECK(it != in.end() && it->payloadJson == "{\"n\":\"\xc3\xa9\"}");
  std::this_thread::sleep_for(std::chrono::milliseconds(1300));
  CHECK_EQ(p.lastAckFromServer(), int64_t(1));  // not processed yet: no ack for 2
  if (it != in.end()) c.MarkProcessed(it->env.seq);
  CHECK(p.WaitFor([&] { return p.lastAckFromServer() >= 2; }, 2500));  // standalone ack within ~1 s
  CHECK_EQ(c.Status().rxSeq, int64_t(2));
  c.Stop();
  p.Stop();
}

// Reconnect with backoff after a close and a TCP drop; resume replays exactly what the
// platform had not durably received; a platform that forgot the stream gets a reset + snapshot.
TEST(TestReconnectResumeReset) {
  mock::Platform p;
  CHECK(p.Start());
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollKey = "rfk_k1_secret";
  Client c(cfg);
  c.SetHelloInfo(Hello());
  std::string err;
  CHECK(c.Start(&err));
  CHECK(WaitState(c, LinkState::Online, 5000));
  json::Value body;
  CHECK(json::Parse(p.lastEnrollBody(), &body) && body.Get("key") && !body.Get("code"));
  const std::string stream = StreamOf(p.MessagesOfType("hello").at(0));
  // 1. close 1001 -> reconnect, resumed
  p.CloseCurrent(1001, "going away");
  CHECK(p.WaitFor([&] { return p.connections() >= 2; }, 3000));
  CHECK(WaitState(c, LinkState::Online, 3000));
  CHECK_EQ(c.Status().sessions, uint32_t(2));
  CHECK_EQ(c.Status().resume, std::string("resumed"));
  // 2. messages the platform receives but never commits: drop, resume from its seq
  p.autoAck = false;
  for (int i = 1; i <= 3; ++i) CHECK(c.Send("event.round_end", "{\"round\":" + std::to_string(i) + "}", 1, true, &err));
  CHECK(p.WaitFor([&] { return p.MessagesOfType("event.round_end").size() == 3; }, 2000));
  CHECK_EQ(c.Status().spoolMsgs, uint32_t(3));
  p.SetPlatformRxSeq(stream, 1);  // only seq 1 was committed
  p.autoAck = true;
  p.DropCurrent();
  CHECK(p.WaitFor([&] { return p.connections() >= 3; }, 3000));
  CHECK(p.WaitFor([&] { return p.MessagesOfType("event.round_end").size() == 5; }, 3000));
  const auto rounds = p.MessagesOfType("event.round_end");
  CHECK_EQ(rounds.at(3).seq(), int64_t(2));  // replay 2 and 3 only
  CHECK_EQ(rounds.at(4).seq(), int64_t(3));
  CHECK_EQ(rounds.at(3).conn, 3);
  const auto hellos = p.MessagesOfType("hello");
  CHECK_EQ(hellos.back().payload()->Get("stream")->Get("last_tx_seq")->AsInt(), int64_t(3));
  CHECK_EQ(StreamOf(hellos.back()), stream);
  CHECK(p.WaitFor([&] { return c.Status().spoolMsgs == 0 && c.Status().ackedSeq == 3; }, 2000));
  // Only the very first session (a stream the platform did not know yet) was a reset.
  CHECK_EQ(p.MessagesOfType("state.snapshot").size(), size_t(1));
  // 3. platform forgot the stream: welcome says reset -> replay, then a state.snapshot
  c.SetState(R"({"match_id":"m1","epoch":2})", "busy");
  p.forgetStreams = true;
  p.CloseCurrent(4503, "deploy");
  CHECK(p.WaitFor([&] { return p.MessagesOfType("state.snapshot").size() >= 2; }, 5000));
  const auto snap = p.MessagesOfType("state.snapshot").back();
  CHECK_EQ(snap.payload()->Get("reason")->AsStr(), std::string("reset"));
  const fleet::json::Value* st = snap.payload()->Get("state");
  CHECK(st && st->IsObj() && st->Get("match_id") && st->Get("match_id")->AsStr() == "m1");
  CHECK_EQ(snap.payload()->Get("availability")->AsStr(), std::string("busy"));
  CHECK_EQ(c.Status().resume, std::string("reset"));
  p.forgetStreams = false;
  // 4. state.request -> snapshot with reason request
  CHECK(p.SendToServer("state.request", "{}", false));
  CHECK(p.WaitFor([&] { return p.MessagesOfType("state.snapshot").size() >= 3; }, 2000));
  CHECK_EQ(p.MessagesOfType("state.snapshot").back().payload()->Get("reason")->AsStr(), std::string("request"));
  c.Stop();
  p.Stop();
}

// Unacked messages survive a process restart: the new client resumes the same stream.
TEST(TestResumeAfterRestart) {
  mock::Platform p;
  CHECK(p.Start());
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollCode = "RUE-AAAA-BBBB-CCCC";
  std::string stream;
  {
    Client c(cfg);
    c.SetHelloInfo(Hello());
    std::string err;
    CHECK(c.Start(&err));
    CHECK(WaitState(c, LinkState::Online, 5000));
    stream = StreamOf(p.MessagesOfType("hello").at(0));
    p.autoAck = false;
    CHECK(c.Send("event.map_result", R"({"map_number":1})", 1, true, &err));
    CHECK(c.Send("event.series_end", R"({"winner":"team1"})", 1, true, &err));
    CHECK(p.WaitFor([&] { return p.MessagesOfType("event.series_end").size() == 1; }, 2000));
    p.SetPlatformRxSeq(stream, 0);  // nothing committed
    c.Stop();
  }
  p.autoAck = true;
  Client c2(cfg);  // same data dir: install_id, credentials, spool
  c2.SetHelloInfo(Hello());
  std::string err;
  CHECK(c2.Start(&err));
  CHECK(WaitState(c2, LinkState::Online, 5000));
  CHECK_EQ(p.enrollments(), 1);  // credentials reused, no second enrollment
  const auto hellos = p.MessagesOfType("hello");
  CHECK_EQ(StreamOf(hellos.back()), stream);
  CHECK_EQ(hellos.back().payload()->Get("stream")->Get("last_tx_seq")->AsInt(), int64_t(2));
  CHECK(p.WaitFor([&] { return p.MessagesOfType("event.series_end").size() == 2; }, 3000));
  CHECK_EQ(p.MessagesOfType("event.map_result").back().seq(), int64_t(1));
  CHECK(p.WaitFor([&] { return c2.Status().spoolMsgs == 0; }, 2000));
  c2.Stop();
  p.Stop();
}

// Offline spooling: messages queued while the platform is down go out after it comes back.
TEST(TestOfflineSpoolThenConnect) {
  mock::Platform p;
  CHECK(p.Start());
  const int port = p.port();
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollCode = "RUE-AAAA-BBBB-CCCC";
  Client c(cfg);
  c.SetHelloInfo(Hello());
  std::string err;
  CHECK(c.Start(&err));
  CHECK(WaitState(c, LinkState::Online, 5000));
  p.Stop();  // platform down
  CHECK(WaitState(c, LinkState::Offline, 3000));
  for (int i = 0; i < 5; ++i) CHECK(c.Send("event.player_ready", "{\"i\":" + std::to_string(i) + "}", 1, true, &err));
  CHECK(p.WaitFor([&] { return c.Status().spoolMsgs == 5; }, 2000));
  CHECK(c.Status().offlineSinceMs > 0);
  mock::Platform p2;
  CHECK(p2.Start(port));
  p2.token = p.token;
  CHECK(p2.WaitFor([&] { return p2.MessagesOfType("event.player_ready").size() == 5; }, 5000));
  CHECK(WaitState(c, LinkState::Online, 2000));
  CHECK_EQ(c.Status().offlineSinceMs, int64_t(0));
  // p2 never saw the stream, so it is a reset: replay then snapshot
  CHECK(p2.WaitFor([&] { return !p2.MessagesOfType("state.snapshot").empty(); }, 2000));
  c.Stop();
  p2.Stop();
}

// No frame for timeout_ms -> the client closes and reconnects.
TEST(TestHeartbeatTimeout) {
  mock::Platform p;
  CHECK(p.Start());
  p.heartbeatIntervalMs = 1000;
  p.heartbeatTimeoutMs = 3000;
  const std::string dir = TempDir();
  ClientConfig cfg = BaseConfig(p, dir);
  cfg.enrollCode = "RUE-AAAA-BBBB-CCCC";
  Client c(cfg);
  c.SetHelloInfo(Hello());
  std::string err;
  CHECK(c.Start(&err));
  CHECK(WaitState(c, LinkState::Online, 5000));
  p.silent = true;
  CHECK(WaitState(c, LinkState::Offline, 6000));
  CHECK(c.Status().lastError.find("no frame") != std::string::npos);
  p.silent = false;
  CHECK(WaitState(c, LinkState::Online, 3000));
  CHECK(p.connections() >= 2);
  c.Stop();
  p.Stop();
}

// 4401 -> rejected; a refused one-time code stops retrying; a bad token never connects.
TEST(TestRejections) {
  {
    mock::Platform p;
    CHECK(p.Start());
    ClientConfig cfg = BaseConfig(p, TempDir());
    cfg.enrollCode = "RUE-AAAA-BBBB-CCCC";
    Client c(cfg);
    c.SetHelloInfo(Hello());
    std::string err;
    CHECK(c.Start(&err));
    CHECK(WaitState(c, LinkState::Online, 5000));
    p.CloseCurrent(4401, "token revoked");
    CHECK(WaitState(c, LinkState::Rejected, 3000));
    CHECK(c.Status().lastError.find("4401") != std::string::npos);
    // A manual reconnect goes straight back (the mock accepts the token again).
    c.RequestReconnect();
    CHECK(WaitState(c, LinkState::Online, 3000));
    c.Stop();
    p.Stop();
  }
  {
    mock::Platform p;
    CHECK(p.Start());
    ClientConfig cfg = BaseConfig(p, TempDir());
    cfg.enrollCode = "RUE-BAD0-BAD0-BAD0";
    Client c(cfg);
    c.SetHelloInfo(Hello());
    std::string err;
    CHECK(c.Start(&err));
    CHECK(WaitState(c, LinkState::Rejected, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK_EQ(p.enrollments(), 1);  // no retry storm with a dead code
    CHECK(c.Status().lastError.find("403") != std::string::npos);
    CHECK(c.Status().lastError.find("BAD0") == std::string::npos);  // code redacted
    // `ru fleet enroll <new code>` path
    c.RequestEnroll("", "RUE-GOOD-GOOD-GOOD");
    CHECK(WaitState(c, LinkState::Online, 5000));
    CHECK_EQ(p.enrollments(), 2);
    c.Stop();
    p.Stop();
  }
  {
    mock::Platform p;
    CHECK(p.Start());
    p.rejectUpgradeStatus = 403;
    ClientConfig cfg = BaseConfig(p, TempDir());
    cfg.enrollCode = "RUE-AAAA-BBBB-CCCC";
    Client c(cfg);
    c.SetHelloInfo(Hello());
    std::string err;
    CHECK(c.Start(&err));
    CHECK(WaitState(c, LinkState::Rejected, 5000));
    CHECK(c.Status().lastError.find("403") != std::string::npos);
    c.Stop();
    p.Stop();
  }
  {
    // Plain ws:// to a public host is refused before any network I/O.
    ClientConfig cfg;
    cfg.url = "http://8.8.8.8:1";
    cfg.insecureDev = true;
    cfg.dataDir = TempDir();
    cfg.enrollCode = "RUE-AAAA-BBBB-CCCC";
    Client c(cfg);
    std::string err;
    CHECK(c.Start(&err));
    CHECK(WaitState(c, LinkState::Rejected, 2000));
    CHECK(c.Status().lastError.find("loopback") != std::string::npos);
    c.Stop();
  }
}

int main() {
  RUN(TestEnrollConnectPingAck);
  RUN(TestReconnectResumeReset);
  RUN(TestResumeAfterRestart);
  RUN(TestOfflineSpoolThenConnect);
  RUN(TestHeartbeatTimeout);
  RUN(TestRejections);
  return ftest::Finish("fleet_integration_test");
}
