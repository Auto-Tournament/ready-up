// Unit tests for demo streaming (fleet_demo.h, FLEET.md §12.2) without a network: the tailer on
// a real growing file, the ack / resume state machine against an in-process fake platform that
// follows protocol/v1/messages/demo.*.json, backpressure (window, a full bulk lane), the final
// pass (header rewritten at the stop, sha256), resume after a new session and after a restart
// (saved state), checksum / refusal / silence, and the guarantee that nothing is ever deleted
// that the platform did not confirm in full.
//   build/plugins/fleet/fleet_demo_test
#include "fleet_demo.h"
#include "fleet_json.h"
#include "fleet_proto.h"
#include "test_util.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace fleet;
using namespace fleet::demo;

namespace {

std::string TempDir() {
  char tmpl[] = "/tmp/fleet_demo_XXXXXX";
  const char* d = mkdtemp(tmpl);
  return d ? d : "/tmp";
}
void RmRf(const std::string& d) { (void)!std::system(("rm -rf '" + d + "'").c_str()); }

std::string Bytes(size_t n, unsigned seed) {
  std::string s(n, '\0');
  uint32_t x = seed * 2654435761u + 1;
  for (auto& c : s) {
    x = x * 1664525u + 1013904223u;
    c = static_cast<char>(x >> 24);
  }
  return s;
}
void Append(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::binary | std::ios::app);
  f << data;
}
void WriteAt(const std::string& path, long off, const std::string& data) {
  FILE* f = std::fopen(path.c_str(), "r+b");
  std::fseek(f, off, SEEK_SET);
  std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
}
std::string ReadAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool Exists(const std::string& p) {
  struct stat st {};
  return stat(p.c_str(), &st) == 0;
}
std::string ShaOf(const std::string& data) {
  Sha256 s;
  s.Update(data.data(), data.size());
  return s.HexDigest();
}
bool B64Decode(const std::string& in, std::string* out) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
  };
  out->clear();
  if (in.size() % 4) return false;
  for (size_t i = 0; i < in.size(); i += 4) {
    uint32_t n = 0;
    int pad = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = in[i + k];
      int v = 0;
      if (c == '=') ++pad;
      else if ((v = val(c)) < 0) return false;
      n = (n << 6) | uint32_t(v);
    }
    out->push_back(char(n >> 16));
    if (pad < 2) out->push_back(char(n >> 8));
    if (pad < 1) out->push_back(char(n));
  }
  return true;
}

struct Sent {
  std::string type, payload;
  int64_t epoch;
  bool bulk;
};

// The link: online / session knobs, everything sent is kept; the bulk lane can be "full".
struct FakeLink {
  bool online = true;
  uint32_t session = 1;
  bool bulkFull = false;
  std::vector<Sent> sent;
  Link Make() {
    Link l;
    l.online = [this] { return online; };
    l.session = [this] { return session; };
    l.send = [this](const std::string& t, const std::string& p, int64_t e) {
      sent.push_back({t, p, e, false});
      return true;
    };
    l.sendBulk = [this](const std::string& t, const std::string& p, int64_t e) {
      if (bulkFull) return false;
      sent.push_back({t, p, e, true});
      return true;
    };
    return l;
  }
  std::vector<Sent> Take() {
    std::vector<Sent> out;
    out.swap(sent);
    return out;
  }
};

// The platform side, as demo.*.json describe it.
struct FakePlatform {
  struct D {
    std::string data;
    int64_t stored = 0;
    bool complete = false;
    int begins = 0;
    bool restartSeen = false;
  };
  std::map<std::string, D> demos;
  bool corruptNextEnd = false;
  int64_t chunkBytes = 0;
  std::vector<int64_t> chunkOffsets;
  std::string refuse;  // answer demo.begin with this error

  std::string Ack(const std::string& id, int64_t off, bool complete = false, const char* err = nullptr) {
    std::string a = "{\"demo_id\":\"" + id + "\",\"offset\":" + std::to_string(off);
    if (complete) a += ",\"complete\":true";
    if (err) a += std::string(",\"error\":{\"code\":\"") + err + "\"}";
    return a + "}";
  }
  // Applies the messages, returns the demo.ack payloads.
  std::vector<std::string> Handle(const std::vector<Sent>& msgs) {
    std::vector<std::string> acks;
    for (const auto& m : msgs) {
      json::Value p;
      if (!json::Parse(m.payload, &p)) continue;
      const std::string id = p.Get("demo_id")->AsStr();
      if (m.type == "demo.begin") {
        if (!refuse.empty()) {
          acks.push_back(Ack(id, 0, false, refuse.c_str()));
          continue;
        }
        D& d = demos[id];
        ++d.begins;
        if (p.Get("restart") && p.Get("restart")->AsBool()) {
          d = D{};
          d.begins = 1;
          d.restartSeen = true;
        }
        acks.push_back(Ack(id, d.stored, d.complete));
        continue;
      }
      auto it = demos.find(id);
      if (it == demos.end()) {
        acks.push_back(Ack(id, 0, false, "unknown_demo"));
        continue;
      }
      D& d = it->second;
      if (m.type == "demo.chunk") {
        const int64_t off = p.Get("offset")->AsInt();
        std::string bytes;
        CHECK(B64Decode(p.Get("data")->AsStr(), &bytes));
        CHECK_EQ(static_cast<int64_t>(bytes.size()), p.Get("size")->AsInt());
        if (off > d.stored) {
          acks.push_back(Ack(id, d.stored, false, "gap"));
          continue;
        }
        if (static_cast<int64_t>(d.data.size()) < off + static_cast<int64_t>(bytes.size())) d.data.resize(off + bytes.size());
        std::memcpy(&d.data[off], bytes.data(), bytes.size());
        d.stored = std::max<int64_t>(d.stored, off + bytes.size());
        chunkBytes += static_cast<int64_t>(bytes.size());
        chunkOffsets.push_back(off);
        acks.push_back(Ack(id, d.stored));
      } else if (m.type == "demo.end") {
        const int64_t size = p.Get("size")->AsInt();
        if (d.stored < size) {
          acks.push_back(Ack(id, d.stored));
          continue;
        }
        d.data.resize(size);
        d.stored = size;
        if (corruptNextEnd) {
          corruptNextEnd = false;
          d.data[0] ^= 0x5A;
        }
        if (ShaOf(d.data) == p.Get("sha256")->AsStr()) {
          d.complete = true;
          acks.push_back(Ack(id, size, true));
        } else {
          d.data.clear();
          d.stored = 0;
          acks.push_back(Ack(id, 0, false, "checksum"));
        }
      }
    }
    return acks;
  }
};

Config SmallConfig() {
  Config c;
  c.chunkBytes = kChunkMin;       // 4 KiB
  c.windowBytes = 4 * kChunkMin;  // 16 KiB
  c.settleMs = 1000;
  c.ackTimeoutMs = 5000;
  c.endRetryMs = 2000;
  c.keepAfterStoredMs = 10000;
  c.staleRecordingMs = 60000;
  return c;
}

// One round trip: tick (sends), the platform answers, the answers are queued for the next tick.
void Pump(Streams& s, FakeLink& link, FakePlatform& plat, int64_t now) {
  s.Tick(link.Make(), now);
  for (const auto& a : plat.Handle(link.Take())) s.OnAck(a);
}

Spec SpecFor(const std::string& path) {
  Spec sp;
  sp.matchId = "ko-r1-m3";
  sp.epoch = 3;
  sp.mapNumber = 2;
  sp.path = path;
  sp.startedAtMs = 1000;
  return sp;
}

int Count(const std::vector<Sent>& v, const std::string& type) {
  int n = 0;
  for (const auto& m : v) n += m.type == type;
  return n;
}

}  // namespace

TEST(TestHelpers) {
  CHECK_EQ(ShaOf(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(ShaOf("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  const std::string big = Bytes(100000, 3);
  Sha256 split;  // incremental in odd pieces == one shot
  for (size_t i = 0; i < big.size(); i += 777) split.Update(big.data() + i, std::min<size_t>(777, big.size() - i));
  CHECK_EQ(split.HexDigest(), ShaOf(big));
  CHECK_EQ(Base64Encode(""), std::string(""));
  CHECK_EQ(Base64Encode("f"), std::string("Zg=="));
  CHECK_EQ(Base64Encode("fo"), std::string("Zm8="));
  CHECK_EQ(Base64Encode("foobar"), std::string("Zm9vYmFy"));
  std::string back;
  CHECK(B64Decode(Base64Encode(big), &back) && back == big);
  CHECK(IsDemoFileName("2026-09-29_ko-r1-m3_de_mirage_A_vs_B.dem"));
  CHECK(!IsDemoFileName("x.txt"));
  CHECK(!IsDemoFileName(".hidden.dem"));
  CHECK(!IsDemoFileName("a b.dem"));
  CHECK(!IsDemoFileName("../x.dem"));
  CHECK(Fnv1a64("a", 1) != Fnv1a64("b", 1) && Fnv1a64("", 0) != 0);
}

// Record, stream while growing (full chunks only, within the window), stop, header rewritten,
// final pass, demo.end, stored, deleted after the keep time.
TEST(TestStreamWholeDemo) {
  const std::string dir = TempDir();
  const std::string path = dir + "/2026-09-29_ko-r1-m3_de_mirage.dem";
  Streams s(SmallConfig(), dir + "/demos.json");
  FakeLink link;
  FakePlatform plat;
  int64_t now = 100000;

  const std::string id = s.Begin(SpecFor(path), now);
  CHECK(IsUlid(id));
  CHECK(s.Begin(SpecFor(dir + "/not a demo.txt"), now).empty());
  CHECK(s.Begin(SpecFor("relative.dem"), now).empty());
  // The file does not exist yet (tv_record runs next frame): nothing happens, nothing fails.
  s.Tick(link.Make(), now);
  CHECK(link.sent.empty());
  CHECK_EQ(s.Unfinished(), size_t(1));

  Append(path, Bytes(10 * 1024, 1));  // 10 KiB: 2 full chunks
  s.Tick(link.Make(), now += 250);
  auto out = link.Take();
  CHECK_EQ(Count(out, "demo.begin"), 1);  // first ask where to resume; no chunk before the answer
  CHECK_EQ(Count(out, "demo.chunk"), 0);
  CHECK(!out.empty() && !out[0].bulk && out[0].epoch == 3);
  json::Value b;
  CHECK(json::Parse(out[0].payload, &b));
  CHECK_EQ(b.Get("demo_id")->AsStr(), id);
  CHECK_EQ(b.Get("match_id")->AsStr(), std::string("ko-r1-m3"));
  CHECK_EQ(b.Get("chunk_size")->AsInt(), int64_t(kChunkMin));
  CHECK(b.Get("recording")->AsBool());
  CHECK(b.Get("restart") == nullptr);
  for (const auto& a : plat.Handle(out)) s.OnAck(a);

  s.Tick(link.Make(), now += 250);
  out = link.Take();
  CHECK_EQ(Count(out, "demo.chunk"), 2);  // the 2 KiB tail waits: while recording, full chunks only
  for (const auto& m : out) CHECK(m.bulk);
  // Window: 40 KiB more but nothing acked -> 16 KiB in flight at most.
  Append(path, Bytes(40 * 1024, 2));
  s.Tick(link.Make(), now += 250);
  out = link.Take();
  CHECK_EQ(Count(out, "demo.chunk"), 2);  // 2 in flight (unacked) + 2 = the 4-chunk window
  // A full bulk lane: nothing is lost, it waits.
  for (const auto& a : plat.Handle(out)) s.OnAck(a);
  link.bulkFull = true;
  s.Tick(link.Make(), now += 250);
  CHECK(link.sent.empty());
  link.bulkFull = false;
  // The platform acks what it got (the first two chunks were not delivered to it yet).
  Pump(s, link, plat, now += 250);
  for (int i = 0; i < 10; ++i) Pump(s, link, plat, now += 250);
  CHECK_EQ(plat.demos[id].stored, int64_t(48 * 1024));  // 50 KiB written, 12 full chunks

  // Stop. CS2 rewrites the header at the stop: bytes the platform already has changed.
  WriteAt(path, 0, "PBDEMS2\0\x11\x22\x33\x44");
  Append(path, Bytes(3000, 4));
  s.End(path, now);
  Pump(s, link, plat, now += 250);
  CHECK(!plat.demos[id].complete);  // not before the file settled
  for (int i = 0; i < 12; ++i) Pump(s, link, plat, now += 250);
  const std::string file = ReadAll(path);
  CHECK(plat.demos[id].complete);
  CHECK(plat.demos[id].data == file);  // the rewritten header was sent again
  CHECK(std::count(plat.chunkOffsets.begin(), plat.chunkOffsets.end(), 0) >= 2);
  auto snap = s.Snapshot();
  CHECK(snap.size() == 1 && snap[0].phase == Phase::Done && snap[0].sha256 == ShaOf(file));
  CHECK_EQ(snap[0].finalSize, static_cast<int64_t>(file.size()));
  CHECK_EQ(s.Unfinished(), size_t(0));
  // Kept for keepAfterStoredMs, then deleted, then forgotten.
  Pump(s, link, plat, now += 5000);
  CHECK(Exists(path));
  Pump(s, link, plat, now += 6000);
  CHECK(!Exists(path));
  CHECK(s.Snapshot().empty());
  RmRf(dir);
}

// A new session asks the platform where it is and continues there (never from zero); after a
// restart the saved state does the same.
TEST(TestResume) {
  const std::string dir = TempDir();
  const std::string path = dir + "/resume.dem";
  Append(path, Bytes(100 * 1024, 5));
  FakeLink link;
  FakePlatform plat;
  int64_t now = 100000;
  std::string id;
  {
    Streams s(SmallConfig(), dir + "/demos.json");
    id = s.Begin(SpecFor(path), now);
    for (int i = 0; i < 4; ++i) Pump(s, link, plat, now += 250);
    const int64_t stored = plat.demos[id].stored;
    CHECK(stored > 0 && stored < 100 * 1024);
    // Reconnect: chunks in flight are lost with the old session.
    link.session = 2;
    s.Tick(link.Make(), now += 250);
    auto out = link.Take();
    CHECK_EQ(Count(out, "demo.begin"), 1);
    CHECK_EQ(Count(out, "demo.chunk"), 0);
    for (const auto& a : plat.Handle(out)) s.OnAck(a);
    s.Tick(link.Make(), now += 250);
    out = link.Take();
    json::Value c;
    CHECK(!out.empty() && json::Parse(out[0].payload, &c));
    CHECK_EQ(c.Get("offset")->AsInt(), stored);  // from the platform's offset
    for (const auto& a : plat.Handle(out)) s.OnAck(a);
    s.Tick(link.Make(), now += 3000);  // saves (dirty, 2 s since the last save)
  }
  // Server restart: a new Streams loads demos.json and resumes.
  const int64_t before = plat.demos[id].stored;
  plat.chunkOffsets.clear();
  Streams s2(SmallConfig(), dir + "/demos.json");
  CHECK(s2.Load(now));
  auto snap = s2.Snapshot();
  CHECK(snap.size() == 1 && snap[0].id == id && snap[0].phase == Phase::Announce);
  link.session = 3;
  for (int i = 0; i < 20; ++i) Pump(s2, link, plat, now += 250);
  CHECK(!plat.chunkOffsets.empty() && plat.chunkOffsets.front() == before);
  CHECK_EQ(plat.demos[id].stored, int64_t(100 * 1024));
  // The recording died with the server: no End(), no growth -> finished after staleRecordingMs.
  for (int i = 0; i < 10; ++i) Pump(s2, link, plat, now += 10000);
  CHECK(plat.demos[id].complete);
  CHECK(plat.demos[id].data == Bytes(100 * 1024, 5));
  CHECK(!Exists(path));  // stored in full, then deleted after keepAfterStoredMs
  RmRf(dir);
}

// Checksum failure -> start over (restart: true); refusal and silence keep the file forever.
TEST(TestFailuresKeepTheFile) {
  const std::string dir = TempDir();
  FakeLink link;
  int64_t now = 100000;
  Config cfg = SmallConfig();
  cfg.keepAfterStoredMs = 1000;
  cfg.forgetFailedMs = 3600 * 1000;
  {
    const std::string path = dir + "/checksum.dem";
    Append(path, Bytes(9000, 6));
    FakePlatform plat;
    plat.corruptNextEnd = true;
    Streams s(cfg, "");
    const std::string id = s.Begin(SpecFor(path), now);
    s.End(path, now);
    for (int i = 0; i < 30; ++i) Pump(s, link, plat, now += 500);
    CHECK(plat.demos[id].restartSeen);
    CHECK(plat.demos[id].complete);
    CHECK(plat.demos[id].data == Bytes(9000, 6));
  }
  {
    const std::string path = dir + "/refused.dem";
    Append(path, Bytes(9000, 7));
    FakePlatform plat;
    plat.refuse = "not_assigned";
    Streams s(cfg, "");
    s.Begin(SpecFor(path), now);
    s.End(path, now);
    for (int i = 0; i < 10; ++i) Pump(s, link, plat, now += 500);
    auto snap = s.Snapshot();
    CHECK(snap.size() == 1 && snap[0].phase == Phase::Refused && snap[0].error == "not_assigned");
    for (int i = 0; i < 10; ++i) Pump(s, link, plat, now += 3600 * 1000);
    CHECK(Exists(path));  // never deleted
    CHECK(s.Snapshot().empty());  // forgotten after forgetFailedMs, the file stays
  }
  {
    // The platform never answers: demo.begin again with backoff, the file stays, still unfinished.
    const std::string path = dir + "/silent.dem";
    Append(path, Bytes(9000, 8));
    Streams s(cfg, "");
    s.Begin(SpecFor(path), now);
    s.End(path, now);
    std::vector<int64_t> begins;
    for (int i = 0; i < 400; ++i) {
      s.Tick(link.Make(), now += 1000);
      for (const auto& m : link.Take()) {
        if (m.type == "demo.begin") begins.push_back(now);
        CHECK(m.type != "demo.chunk");
      }
    }
    CHECK(begins.size() >= 4 && begins.size() < 40);
    CHECK(begins.size() >= 3 && begins[2] - begins[1] > begins[1] - begins[0]);  // backing off
    CHECK(Exists(path));
    CHECK_EQ(s.Unfinished(), size_t(1));
  }
  RmRf(dir);
}

// Without a platform nothing happens to demos: offline forever, never deleted; files never
// handed over are never touched; the file replaced mid-way restarts the stream.
TEST(TestNoPlatformNoChanges) {
  const std::string dir = TempDir();
  const std::string handed = dir + "/handed.dem";
  const std::string other = dir + "/other.dem";
  Append(handed, Bytes(20000, 9));
  Append(other, Bytes(20000, 10));
  Config cfg = SmallConfig();
  cfg.keepAfterStoredMs = 1;
  FakeLink link;
  link.online = false;
  int64_t now = 100000;
  Streams s(cfg, dir + "/demos.json");
  s.Begin(SpecFor(handed), now);
  s.End(handed, now);
  for (int i = 0; i < 100; ++i) s.Tick(link.Make(), now += 24 * 3600 * 1000);
  CHECK(link.sent.empty());
  CHECK(Exists(handed) && Exists(other));
  CHECK(ReadAll(other) == Bytes(20000, 10));
  CHECK_EQ(s.Unfinished(), size_t(1));

  // Truncated / replaced while streaming: start over with restart: true.
  const std::string rot = dir + "/rotate.dem";
  Append(rot, Bytes(20000, 11));
  FakeLink l2;
  FakePlatform plat;
  Streams s2(SmallConfig(), "");
  const std::string id = s2.Begin(SpecFor(rot), now);
  for (int i = 0; i < 6; ++i) Pump(s2, l2, plat, now += 250);
  CHECK(plat.demos[id].stored > 0);
  unlink(rot.c_str());
  Append(rot, Bytes(9000, 12));
  s2.Tick(l2.Make(), now += 250);
  auto out = l2.Take();
  CHECK_EQ(Count(out, "demo.begin"), 1);
  json::Value b;
  CHECK(!out.empty() && json::Parse(out[0].payload, &b) && b.Get("restart") && b.Get("restart")->AsBool());
  RmRf(dir);
}

int main() {
  RUN(TestHelpers);
  RUN(TestStreamWholeDemo);
  RUN(TestResume);
  RUN(TestFailuresKeepTheFile);
  RUN(TestNoPlatformNoChanges);
  return ftest::Finish("fleet_demo_test");
}
