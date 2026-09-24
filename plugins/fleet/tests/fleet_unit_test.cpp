// Unit tests for the fleet link's I/O-free parts: JSON, envelope, ULID, backoff, close codes,
// redaction, URL rules, inbound seq/ack tracking, the disk spool and the credentials file.
//   build/fleet_unit_test
#include "fleet_json.h"
#include "fleet_proto.h"
#include "fleet_spool.h"
#include "fleet_store.h"
#include "test_util.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>

using namespace fleet;

namespace {
std::string TempDir() {
  char tmpl[] = "/tmp/fleet_unit_XXXXXX";
  const char* d = mkdtemp(tmpl);
  return d ? d : "/tmp";
}
void RmRf(const std::string& d) {
  const std::string cmd = "rm -rf '" + d + "'";
  (void)!std::system(cmd.c_str());
}
}  // namespace

TEST(TestJsonRoundTrip) {
  json::Value v;
  std::string err;
  CHECK(json::Parse(R"({"b":1,"a":"x\u00e9\ud83d\ude00\n","n":-9007199254740993,"f":1.5,"z":[true,null,{}]})", &v, &err));
  CHECK_EQ(v.o[0].first, std::string("b"));  // key order kept
  CHECK_EQ(v.Get("a")->s, std::string("x\xc3\xa9\xf0\x9f\x98\x80\n"));
  CHECK_EQ(v.Get("n")->i, static_cast<int64_t>(-9007199254740993LL));  // exact beyond 2^53
  CHECK(v.Get("f")->t == json::Value::T::Num);
  CHECK_EQ(json::Dump(v), std::string(R"({"b":1,"a":"x)"
                                      "\xc3\xa9\xf0\x9f\x98\x80"
                                      R"(\n","n":-9007199254740993,"f":1.5,"z":[true,null,{}]})"));
  CHECK(!json::Parse("{\"a\":1,}", &v));
  CHECK(!json::Parse("{\"a\":\"\x01\"}", &v));
  CHECK(!json::Parse("[1] x", &v));
  std::string deep(100, '[');
  deep += std::string(100, ']');
  CHECK(!json::Parse(deep, &v));  // depth limit
}

TEST(TestUlid) {
  const std::string a = NewUlid(1790340012345);
  const std::string b = NewUlid(1790340012346);
  CHECK_EQ(a.size(), size_t(26));
  CHECK(IsUlid(a));
  CHECK(a < b);  // lexicographic = time order
  CHECK(NewUlid(1) != NewUlid(1));
  CHECK_EQ(NewUlid(0).substr(0, 10), std::string("0000000000"));
  CHECK(!IsUlid("01J8ZQ4T8W6N3X0F2R5K7M9P1I"));  // I is not Crockford
}

TEST(TestTypes) {
  CHECK(IsValidType("event.round_end"));
  CHECK(IsValidType("ping"));
  CHECK(IsValidType("state.request"));
  CHECK(!IsValidType("Event.x"));
  CHECK(!IsValidType("event..x"));
  CHECK(!IsValidType(".x"));
  CHECK(!IsValidType("x."));
  CHECK(!IsValidType("_x"));
  CHECK(IsEphemeralType("pong"));
  CHECK(!IsEphemeralType("event.phase"));
  CHECK(IsCriticalType("event.round_end"));
  CHECK(IsCriticalType("cmd.result"));
  CHECK(!IsCriticalType("event.player_ready"));
}

TEST(TestEnvelope) {
  Envelope e;
  e.type = "event.round_end";
  e.id = NewUlid(1000);
  e.seq = 1842;
  e.ack = 77;
  e.ts = 1790340012345;
  e.epoch = 3;
  e.payload.Set("round", json::Value::Int(14));
  const std::string text = Encode(e);
  Envelope d;
  std::string err;
  CHECK(Decode(text, &d, &err));
  CHECK_EQ(d.type, e.type);
  CHECK_EQ(d.id, e.id);
  CHECK_EQ(d.seq, int64_t(1842));
  CHECK_EQ(d.ack, int64_t(77));
  CHECK_EQ(d.epoch, int64_t(3));
  CHECK_EQ(d.ts, int64_t(1790340012345));
  CHECK(d.ref.empty());
  CHECK_EQ(d.payload.Get("round")->i, int64_t(14));
  // Ephemeral: no seq, no epoch, ref present.
  Envelope p;
  p.type = "pong";
  p.id = NewUlid(1);
  p.ts = 5;
  p.ref = "01J8ZQ4T8W6N3X0F2R5K7M9P1C";
  const std::string pt = EncodeRaw(p, R"({"t":5})");
  CHECK(pt.find("\"seq\"") == std::string::npos);
  CHECK(pt.find("\"ack\"") == std::string::npos);
  CHECK(pt.find("\"ref\":\"01J8ZQ4T8W6N3X0F2R5K7M9P1C\"") != std::string::npos);
  CHECK(Decode(pt, &d, &err));
  CHECK_EQ(d.seq, int64_t(0));
  CHECK_EQ(d.ack, int64_t(-1));
  // Rejects.
  CHECK(!Decode(R"({"v":1,"type":"x.y","id":"a","ts":1})", &d, &err));                  // no payload
  CHECK(!Decode(R"({"v":1,"type":"X","id":"a","ts":1,"payload":{}})", &d, &err));         // bad type
  CHECK(!Decode(R"({"v":1,"type":"x.y","seq":0,"ts":1,"payload":{}})", &d, &err));        // seq >= 1
  CHECK(!Decode(R"({"v":1,"type":"x.y","ts":1,"payload":[]})", &d, &err));                // payload object
  CHECK(Decode(R"({"v":1,"type":"x.y","ts":1,"payload":{},"extra":42})", &d, &err));      // unknown ignored
  std::string big = R"({"v":1,"type":"x.y","ts":1,"payload":{"s":")" + std::string(kMaxFrameBytes, 'a') + "\"}}";
  CHECK(!Decode(big, &d, &err));
}

TEST(TestBackoffSchedule) {
  Backoff b(BackoffPolicy{}, 42);
  const int64_t want[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
  for (int64_t w : want) {
    CHECK_EQ(b.Ceiling(), w);
    const int64_t d = b.Next();
    CHECK(d >= 0 && d <= w);
  }
  // Full jitter spreads: 200 draws at the cap are not all the same and stay in range.
  std::set<int64_t> seen;
  for (int i = 0; i < 200; ++i) {
    const int64_t d = b.Next();
    CHECK(d >= 0 && d <= 30000);
    seen.insert(d / 1000);
  }
  CHECK(seen.size() > 10);
  // A short session keeps the attempt count, a 60 s one resets it.
  b.OnSessionEnded(59000);
  CHECK_EQ(b.Ceiling(), int64_t(30000));
  b.OnSessionEnded(60000);
  CHECK_EQ(b.Ceiling(), int64_t(1000));
  // 4401/4403: cap 10 min; 4503: base 2 s.
  Backoff r(BackoffPolicy{}, 1);
  for (int i = 0; i < 20; ++i) r.Next(600000);
  CHECK_EQ(r.Ceiling(600000), int64_t(600000));
  Backoff dr(BackoffPolicy{}, 1);
  CHECK_EQ(dr.Ceiling(0, 2000), int64_t(2000));
  dr.Next(0, 2000);
  CHECK_EQ(dr.Ceiling(0, 2000), int64_t(4000));
}

TEST(TestCloseCodes) {
  CloseAction a = ClassifyClose(1000, "");
  CHECK(!a.rejected && a.fixedDelayMs < 0 && a.capMs == 0);
  a = ClassifyClose(4401, "bad token");
  CHECK(a.rejected);
  CHECK_EQ(a.capMs, int64_t(600000));
  a = ClassifyClose(4403, "");
  CHECK(a.rejected);
  a = ClassifyClose(4409, "");
  CHECK_EQ(a.fixedDelayMs, int64_t(30000));
  a = ClassifyClose(4426, "");
  CHECK(a.versionUnsupported);
  CHECK_EQ(a.capMs, int64_t(600000));
  a = ClassifyClose(4429, R"({"retry_after_ms":4500})");
  CHECK_EQ(a.fixedDelayMs, int64_t(4500));
  a = ClassifyClose(4429, "");
  CHECK_EQ(a.fixedDelayMs, int64_t(30000));
  a = ClassifyClose(4503, "");
  CHECK_EQ(a.baseMs, int64_t(2000));
  a = ClassifyClose(4400, "schema: rus_ABCDEFGHIJKL_c2VjcmV0 leaked");
  CHECK(a.what.find("c2VjcmV0") == std::string::npos);
  a = ClassifyHttpStatus(401, 0);
  CHECK(a.rejected);
  a = ClassifyHttpStatus(429, 7000);
  CHECK_EQ(a.fixedDelayMs, int64_t(7000));
  a = ClassifyHttpStatus(502, 0);
  CHECK(!a.rejected && a.fixedDelayMs < 0);
}

TEST(TestRedaction) {
  const std::string s = Redact(
      "Authorization: Bearer rus_ABCDEFGHIJKL_c2VjcmV0LXNlY3JldA code=RUE-7F3K-9QX2-LM4D key rfk_k1_s3cr3t host "
      "trust_me");
  CHECK(s.find("c2VjcmV0") == std::string::npos);
  CHECK(s.find("7F3K") == std::string::npos);
  CHECK(s.find("s3cr3t") == std::string::npos);
  CHECK(s.find("trust_me") != std::string::npos);  // "trust_" is not a token prefix
  CHECK(s.find("rus_***") != std::string::npos || s.find("Bearer ***") != std::string::npos);
}

TEST(TestUrls) {
  CHECK_EQ(JoinUrl("https://at.example.com/", "/api/fleet/ws", true), std::string("wss://at.example.com/api/fleet/ws"));
  CHECK_EQ(JoinUrl("http://127.0.0.1:8080", "/api/fleet/enroll", false),
           std::string("http://127.0.0.1:8080/api/fleet/enroll"));
  CHECK(CheckUrlAllowed("https://at.example.com", false).empty());
  CHECK(CheckUrlAllowed("wss://at.example.com/api/fleet/ws", false).empty());
  CHECK(!CheckUrlAllowed("http://127.0.0.1:3000", false).empty());  // needs insecure_dev
  CHECK(CheckUrlAllowed("http://127.0.0.1:3000", true).empty());
  CHECK(CheckUrlAllowed("ws://192.168.1.20:3000", true).empty());
  CHECK(CheckUrlAllowed("ws://[::1]:3000", true).empty());
  CHECK(CheckUrlAllowed("http://localhost:3000", true).empty());
  CHECK(!CheckUrlAllowed("http://8.8.8.8", true).empty());          // public host: never plain
  CHECK(!CheckUrlAllowed("http://172.32.0.1", true).empty());       // outside 172.16/12
  CHECK(CheckUrlAllowed("http://172.31.255.1", true).empty());
  CHECK(!CheckUrlAllowed("ftp://x", true).empty());
}

TEST(TestRxTracking) {
  RxTracker rx;
  rx.Reset(0);
  CHECK(rx.OnReliable(1, "A") == RxTracker::Verdict::Accept);
  CHECK(rx.OnReliable(2, "B") == RxTracker::Verdict::Accept);
  CHECK(rx.OnReliable(2, "B") == RxTracker::Verdict::Duplicate);
  CHECK(rx.OnReliable(4, "D") == RxTracker::Verdict::OutOfOrder);
  CHECK(rx.OnReliable(3, "A") == RxTracker::Verdict::Duplicate);  // same id, new seq: replayed copy
  CHECK(rx.OnReliable(3, "C") == RxTracker::Verdict::Accept);
  CHECK_EQ(rx.received(), int64_t(3));
  // Done out of order: processed only advances over the contiguous prefix.
  CHECK(!rx.MarkDone(2, 1000));
  CHECK_EQ(rx.processed(), int64_t(0));
  CHECK(rx.MarkDone(1, 1000));
  CHECK_EQ(rx.processed(), int64_t(2));
  CHECK(!rx.AckDue(1500));  // < 1 s and < 32
  CHECK_EQ(rx.NextAckDueMs(), int64_t(2000));
  CHECK(rx.AckDue(2000));
  rx.OnAckSent(2, 2000);
  CHECK(!rx.AckDue(5000));
  CHECK(rx.MarkDone(3, 6000));
  CHECK_EQ(rx.processed(), int64_t(3));
  // 32 processed messages force an ack at once.
  for (int64_t s = 4; s <= 35; ++s) {
    CHECK(rx.OnReliable(s, "id" + std::to_string(s)) == RxTracker::Verdict::Accept);
    rx.MarkDone(s, 6001);
  }
  CHECK(rx.AckDue(6001));
  // Ephemeral dedupe by id.
  CHECK(rx.OnEphemeralId("P1"));
  CHECK(!rx.OnEphemeralId("P1"));
  // After a restart the persisted processed seq is the new baseline.
  RxTracker r2;
  r2.Reset(35);
  CHECK(r2.OnReliable(35, "x") == RxTracker::Verdict::Duplicate);
  CHECK(r2.OnReliable(36, "y") == RxTracker::Verdict::Accept);
}

TEST(TestSpoolPersistence) {
  const std::string dir = TempDir() + "/spool";
  std::string err, note;
  std::string streamId;
  {
    Spool s;
    CHECK(s.Open(dir, Spool::Limits{}, &err, &note));
    streamId = s.streamId();
    CHECK(IsUlid(streamId));
    CHECK_EQ(s.Append("event.phase", NewUlid(1), "", 1, 1, R"({"to":"live"})", &err), int64_t(1));
    CHECK_EQ(s.Append("event.round_end", NewUlid(2), "", 2, 1, R"({"round":1,"name":"\u00e9"})", &err), int64_t(2));
    CHECK_EQ(s.Append("cmd.result", NewUlid(3), "01J8ZQ4T8W6N3X0F2R5K7M9P1C", 3, 1, R"({"status":"ok"})", &err),
             int64_t(3));
    s.AckUpTo(1);
    s.SetRxSeq(9);
    CHECK_EQ(s.count(), size_t(2));
    s.Close();
  }
  struct stat st {};
  CHECK(stat((dir + "/stream.log").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
  {
    Spool s;
    CHECK(s.Open(dir, Spool::Limits{}, &err, &note));
    CHECK_EQ(s.streamId(), streamId);  // same stream across a restart
    CHECK_EQ(s.count(), size_t(2));
    CHECK_EQ(s.ackedSeq(), int64_t(1));
    CHECK_EQ(s.rxSeq(), int64_t(9));
    CHECK_EQ(s.records().front().seq, int64_t(2));
    CHECK_EQ(s.records().front().payload, std::string("{\"round\":1,\"name\":\"\xc3\xa9\"}"));
    CHECK_EQ(s.records().back().ref, std::string("01J8ZQ4T8W6N3X0F2R5K7M9P1C"));
    CHECK_EQ(s.Append("event.phase", NewUlid(4), "", 4, 1, "{}", &err), int64_t(4));
    CHECK(!s.gap());
    s.AckUpTo(4);
    CHECK_EQ(s.count(), size_t(0));
    s.Close();
  }
  {
    // A torn last line (crash mid-write) is skipped and flags a gap.
    std::ofstream f(dir + "/stream.log", std::ios::app);
    f << "{\"seq\":5,\"type\":\"event.phase\",\"id\":\"x\",\"ts\":5,\"epoch\":1,\"crit\":false,\"payl";
  }
  {
    Spool s;
    CHECK(s.Open(dir, Spool::Limits{}, &err, &note));
    CHECK(s.gap());
    const std::string before = s.streamId();
    CHECK(s.RotateIfGap());
    CHECK(s.streamId() != before);
    CHECK(!s.gap());
    CHECK_EQ(s.lastSeq(), int64_t(0));
  }
  RmRf(dir.substr(0, dir.size() - 6));
}

TEST(TestSpoolLimits) {
  const std::string dir = TempDir() + "/spool";
  std::string err, note;
  Spool s;
  Spool::Limits lim;
  lim.maxMsgs = 10;
  CHECK(s.Open(dir, lim, &err, &note));
  // 3 critical + 7 compactable fill it.
  for (int i = 0; i < 3; ++i) CHECK(s.Append("event.round_end", NewUlid(i), "", i, 1, "{}", &err) > 0);
  for (int i = 0; i < 7; ++i) CHECK(s.Append("event.player_ready", NewUlid(i), "", i, 1, "{}", &err) > 0);
  CHECK_EQ(s.count(), size_t(10));
  CHECK(!s.gap());
  // One more compactable: the oldest compactables are dropped (to 90 %), criticals stay.
  CHECK(s.Append("event.player_ready", NewUlid(11), "", 11, 1, "{}", &err) > 0);
  CHECK(s.gap());
  CHECK(s.count() <= size_t(10));
  size_t crit = 0;
  for (const auto& r : s.records()) crit += r.critical;
  CHECK_EQ(crit, size_t(3));
  // Only criticals left: new compactables are refused, criticals still fit (up to 2x).
  for (int i = 0; i < 20 && s.count() > crit; ++i) {
    s.Append("event.round_end", NewUlid(100 + i), "", 1, 1, "{}", &err);
    crit = 0;
    for (const auto& r : s.records()) crit += r.critical;
  }
  CHECK(s.count() >= size_t(10));
  CHECK_EQ(s.Append("event.player_ready", NewUlid(200), "", 1, 1, "{}", &err), int64_t(0));
  CHECK(err.find("spool full") != std::string::npos);
  int64_t accepted = 0;
  for (int i = 0; i < 30; ++i) accepted += s.Append("event.round_end", NewUlid(300 + i), "", 1, 1, "{}", &err) > 0;
  CHECK(s.count() <= size_t(20));  // hard stop at 2x
  CHECK(accepted < 30);
  RmRf(dir.substr(0, dir.size() - 6));
}

TEST(TestCredentialsFile) {
  const std::string dir = TempDir();
  Credentials c;
  c.serverId = "srv_1";
  c.token = "rus_ABCDEFGHIJKL_c2VjcmV0";
  c.wsUrl = "wss://x/api/fleet/ws";
  c.url = "https://x";
  c.installId = "01J8ZQ4T8W6N3X0F2R5K7M9P1C";
  c.enrolledAt = 123;
  std::string err;
  umask(022);
  CHECK(SaveCredentials(dir + "/credentials.json", c, &err));
  struct stat st {};
  CHECK(stat((dir + "/credentials.json").c_str(), &st) == 0);
  CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0600);
  Credentials r;
  CHECK(LoadCredentials(dir + "/credentials.json", &r, &err));
  CHECK_EQ(r.token, c.token);
  CHECK_EQ(r.serverId, c.serverId);
  CHECK_EQ(r.enrolledAt, int64_t(123));
  // Loosened by hand: tightened again on load.
  chmod((dir + "/credentials.json").c_str(), 0644);
  CHECK(LoadCredentials(dir + "/credentials.json", &r, &err));
  stat((dir + "/credentials.json").c_str(), &st);
  CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0600);
  CHECK(!LoadCredentials(dir + "/missing.json", &r, &err));
  CHECK_EQ(err, std::string("missing"));
  const std::string id1 = LoadOrCreateInstallId(dir, &err);
  const std::string id2 = LoadOrCreateInstallId(dir, &err);
  CHECK(IsUlid(id1));
  CHECK_EQ(id1, id2);
  RmRf(dir);
}

int main() {
  RUN(TestJsonRoundTrip);
  RUN(TestUlid);
  RUN(TestTypes);
  RUN(TestEnvelope);
  RUN(TestBackoffSchedule);
  RUN(TestCloseCodes);
  RUN(TestRedaction);
  RUN(TestUrls);
  RUN(TestRxTracking);
  RUN(TestSpoolPersistence);
  RUN(TestSpoolLimits);
  RUN(TestCredentialsFile);
  return ftest::Finish("fleet_unit_test");
}
