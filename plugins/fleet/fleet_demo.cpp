#include "fleet_demo.h"

#include "fleet_json.h"
#include "fleet_proto.h"
#include "fleet_store.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fleet::demo {

// ---------------------------------------------------------------------------- helpers

namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

bool ReadRange(const std::string& path, int64_t offset, int64_t len, std::string* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out->assign(static_cast<size_t>(len), '\0');
  bool ok = fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0 &&
            std::fread(&(*out)[0], 1, static_cast<size_t>(len), f) == static_cast<size_t>(len);
  std::fclose(f);
  return ok;
}

std::string Hex64(uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
  return b;
}

std::string Basename(const std::string& p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

}  // namespace

Sha256::Sha256() {
  static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memcpy(h_, init, sizeof(h_));
}

void Sha256::Block(const unsigned char* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (uint32_t{p[i * 4]} << 24) | (uint32_t{p[i * 4 + 1]} << 16) | (uint32_t{p[i * 4 + 2]} << 8) |
           uint32_t{p[i * 4 + 3]};
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = hh + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
  h_[5] += f;
  h_[6] += g;
  h_[7] += hh;
}

void Sha256::Update(const void* data, size_t len) {
  const auto* p = static_cast<const unsigned char*>(data);
  total_ += len;
  if (bufLen_ > 0) {
    const size_t take = std::min(len, sizeof(buf_) - bufLen_);
    std::memcpy(buf_ + bufLen_, p, take);
    bufLen_ += take;
    p += take;
    len -= take;
    if (bufLen_ == sizeof(buf_)) {
      Block(buf_);
      bufLen_ = 0;
    }
  }
  while (len >= 64) {
    Block(p);
    p += 64;
    len -= 64;
  }
  if (len > 0) {
    std::memcpy(buf_, p, len);
    bufLen_ = len;
  }
}

std::string Sha256::HexDigest() {
  const uint64_t bits = total_ * 8;
  const unsigned char pad = 0x80;
  Update(&pad, 1);
  const unsigned char zero = 0;
  while (bufLen_ != 56) Update(&zero, 1);
  unsigned char len[8];
  for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(bits >> (56 - i * 8));
  Update(len, 8);
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (uint32_t v : h_) {
    for (int i = 28; i >= 0; i -= 4) out.push_back(kHex[(v >> i) & 0xf]);
  }
  return out;
}

std::string Base64Encode(const std::string& in) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(in.data());
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (uint32_t{p[i]} << 16) | (uint32_t{p[i + 1]} << 8) | p[i + 2];
    out.push_back(k[(v >> 18) & 63]);
    out.push_back(k[(v >> 12) & 63]);
    out.push_back(k[(v >> 6) & 63]);
    out.push_back(k[v & 63]);
  }
  if (i < in.size()) {
    uint32_t v = uint32_t{p[i]} << 16;
    if (i + 1 < in.size()) v |= uint32_t{p[i + 1]} << 8;
    out.push_back(k[(v >> 18) & 63]);
    out.push_back(k[(v >> 12) & 63]);
    out.push_back(i + 1 < in.size() ? k[(v >> 6) & 63] : '=');
    out.push_back('=');
  }
  return out;
}

uint64_t Fnv1a64(const void* data, size_t len) {
  uint64_t h = 1469598103934665603ull;
  const auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h ? h : 1;  // 0 means "unknown" in Stream::fps
}

bool IsDemoFileName(const std::string& n) {
  if (n.size() < 5 || n.size() > 200 || n.compare(n.size() - 4, 4, ".dem") != 0 || n[0] == '.') return false;
  for (unsigned char c : n) {
    if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
  }
  return true;
}

const char* PhaseName(Phase p) {
  switch (p) {
    case Phase::Announce: return "announce";
    case Phase::Stream: return "stream";
    case Phase::Done: return "stored";
    case Phase::Refused: return "refused";
    case Phase::Failed: return "failed";
  }
  return "?";
}

namespace {

Phase PhaseFromName(const std::string& n) {
  if (n == "stream") return Phase::Stream;
  if (n == "stored") return Phase::Done;
  if (n == "refused") return Phase::Refused;
  if (n == "failed") return Phase::Failed;
  return Phase::Announce;
}

}  // namespace

// ---------------------------------------------------------------------------- Streams

Streams::Streams(Config cfg, std::string statePath, std::function<void(int, const std::string&)> log)
    : cfg_(cfg), statePath_(std::move(statePath)), log_(std::move(log)) {
  cfg_.chunkBytes = std::max(kChunkMin, std::min(kChunkMax, cfg_.chunkBytes));
  cfg_.windowBytes = std::max(cfg_.windowBytes, cfg_.chunkBytes);
}

void Streams::Log(int level, const std::string& msg) const {
  if (log_) log_(level, msg);
}

std::string Streams::Begin(const Spec& spec, int64_t nowMs) {
  const std::string file = Basename(spec.path);
  if (spec.path.empty() || spec.path[0] != '/' || !IsDemoFileName(file) || spec.matchId.empty()) {
    Log(1, "fleet: demo " + file + ": not streamed (needs an absolute path to a .dem file and a match id)");
    return {};
  }
  Cmd c;
  c.kind = Cmd::BeginK;
  c.begin.id = NewUlid(nowMs);
  c.begin.spec = spec;
  if (c.begin.spec.startedAtMs <= 0) c.begin.spec.startedAtMs = nowMs;
  c.begin.file = file;
  c.begin.chunk = cfg_.chunkBytes;
  c.nowMs = nowMs;
  const std::string id = c.begin.id;
  std::lock_guard<std::mutex> lk(qmu_);
  queue_.push_back(std::move(c));
  return id;
}

void Streams::End(const std::string& path, int64_t nowMs) {
  Cmd c;
  c.kind = Cmd::EndK;
  c.text = path;
  c.nowMs = nowMs;
  std::lock_guard<std::mutex> lk(qmu_);
  queue_.push_back(std::move(c));
}

void Streams::OnAck(const std::string& payloadJson) {
  Cmd c;
  c.kind = Cmd::AckK;
  c.text = payloadJson;
  std::lock_guard<std::mutex> lk(qmu_);
  queue_.push_back(std::move(c));
}

void Streams::Apply(const Cmd& c, const Link& link, int64_t nowMs) {
  if (c.kind == Cmd::BeginK) {
    for (auto& s : streams_) {
      if (s.spec.path == c.begin.spec.path && (s.phase == Phase::Announce || s.phase == Phase::Stream)) {
        Log(1, "fleet: demo " + s.file + " was still streaming; a new recording replaces it");
        s.phase = Phase::Failed;
        s.error = "replaced";
        s.doneAtMs = nowMs;
      }
    }
    streams_.push_back(c.begin);
    Log(0, "fleet: demo " + c.begin.file + ": streaming to the platform while it records (demo_id " + c.begin.id + ")");
    dirty_ = true;
    return;
  }
  if (c.kind == Cmd::EndK) {
    for (auto& s : streams_) {
      if (s.spec.path == c.text && s.recording) {
        s.recording = false;
        s.stoppedAtMs = c.nowMs;
        dirty_ = true;
      }
    }
    return;
  }
  ApplyAck(c.text, link, nowMs);
}

void Streams::ApplyAck(const std::string& payload, const Link& link, int64_t nowMs) {
  json::Value v;
  if (!json::Parse(payload, &v) || !v.IsObj()) return;
  const std::string id = v.Get("demo_id") ? v.Get("demo_id")->AsStr() : "";
  auto it = std::find_if(streams_.begin(), streams_.end(), [&](const Stream& s) { return s.id == id; });
  if (it == streams_.end()) return;
  Stream& s = *it;
  if (s.phase == Phase::Done || s.phase == Phase::Refused || s.phase == Phase::Failed) return;
  const json::Value* ov = v.Get("offset");
  const int64_t offset = ov && ov->t == json::Value::T::Int && ov->i >= 0 ? ov->i : -1;
  dirty_ = true;
  if (const json::Value* e = v.Get("error"); e && e->IsObj()) {
    const std::string code = e->Get("code") ? e->Get("code")->AsStr() : "error";
    s.error = code;
    if (code == "gap" && offset >= 0) {
      s.sent = offset;
      s.acked = offset;
      s.lastAckMs = nowMs;
    } else if (code == "checksum") {
      if (++s.checksumFailures > cfg_.maxChecksumRetries) {
        s.phase = Phase::Failed;
        s.doneAtMs = nowMs;
        Log(1, "fleet: demo " + s.file + ": the platform's copy failed the sha256 check " +
                   std::to_string(s.checksumFailures) + " times; kept locally, not streamed again");
        return;
      }
      Log(1, "fleet: demo " + s.file + ": the platform's copy failed the sha256 check; sending it again from the start");
      s.restart = true;
      s.phase = Phase::Announce;
      s.announcedMs = 0;
      s.announceTries = 0;
      s.sent = s.acked = 0;
      s.fps.clear();
      s.rewrites.clear();
      s.endSentMs = 0;
    } else if (code == "not_assigned" || code == "stale_epoch" || code == "too_large" || code == "refused") {
      s.phase = Phase::Refused;
      s.doneAtMs = nowMs;
      Log(1, "fleet: demo " + s.file + ": the platform refused it (" + code + "); kept locally");
    } else {  // unknown_demo (the platform lost it), storage (try later), anything else
      s.phase = Phase::Announce;
      if (code == "unknown_demo") {
        s.announcedMs = 0;
        s.announceTries = 0;
      }
    }
    return;
  }
  if (offset < 0) return;
  const bool complete = v.Get("complete") && v.Get("complete")->AsBool();
  s.error.clear();
  if (s.phase == Phase::Announce) {
    // The answer to demo.begin: continue from what the platform has, never from zero.
    s.acked = s.sent = offset;
    s.session = link.session ? link.session() : 0;
    s.phase = Phase::Stream;
    s.announceTries = 0;
    s.restart = false;
    s.endSentMs = 0;
    s.lastAckMs = nowMs;
    Log(3, "fleet: demo " + s.file + ": platform has " + std::to_string(offset) + " bytes; streaming from there");
  } else {
    if (offset < s.acked) s.sent = offset;  // the platform lost bytes: send them again
    s.acked = offset;
    if (offset > s.sent) s.sent = offset;
    s.lastAckMs = nowMs;
  }
  if (complete && s.finalSize >= 0 && offset == s.finalSize) {
    s.phase = Phase::Done;
    s.doneAtMs = nowMs;
    Log(0, "fleet: demo " + s.file + ": the platform stored it (" + std::to_string(s.finalSize) + " bytes, sha256 " +
               s.sha256.substr(0, 12) + "...)" +
               (cfg_.keepAfterStoredMs > 0
                    ? "; the local copy is deleted in " + std::to_string(cfg_.keepAfterStoredMs / 3600000) + " h"
                    : "; the local copy is kept (demo_keep_hours=0)"));
  }
}

bool Streams::FinalPass(Stream& s) {
  FILE* f = std::fopen(s.spec.path.c_str(), "rb");
  if (!f) return false;
  Sha256 sha;
  std::string buf(static_cast<size_t>(s.chunk), '\0');
  int64_t total = 0;
  std::vector<int64_t> rewrites;
  for (int64_t i = 0;; ++i) {
    const size_t n = std::fread(&buf[0], 1, buf.size(), f);
    if (n == 0) break;
    sha.Update(buf.data(), n);
    const int64_t off = total;
    total += static_cast<int64_t>(n);
    // Bytes already sent that are not what the file holds now (CS2 rewrites the demo header when
    // the recording stops) or whose fingerprint is unknown: send those chunks again.
    if (off < s.sent) {
      const bool full = static_cast<int64_t>(n) == s.chunk;
      const uint64_t fp = full ? Fnv1a64(buf.data(), n) : 0;
      const bool known = full && static_cast<size_t>(i) < s.fps.size() && s.fps[static_cast<size_t>(i)] != 0;
      if (!known || s.fps[static_cast<size_t>(i)] != fp) rewrites.push_back(off);
    }
    if (n < buf.size()) break;
  }
  const bool err = std::ferror(f) != 0;
  std::fclose(f);
  if (err) return false;
  s.finalSize = total;
  s.sha256 = sha.HexDigest();
  s.rewrites = std::move(rewrites);
  if (s.sent > total) s.sent = total;
  Log(0, "fleet: demo " + s.file + ": recording finished, " + std::to_string(total) + " bytes" +
             (s.rewrites.empty() ? std::string() : ", " + std::to_string(s.rewrites.size()) + " chunk(s) changed since sent"));
  return true;
}

void Streams::Work(Stream& s, const Link& link, int64_t nowMs) {
  if (s.phase == Phase::Done) {
    if (cfg_.keepAfterStoredMs > 0 && nowMs - s.doneAtMs >= cfg_.keepAfterStoredMs && s.error != "deleted") {
      struct stat st {};
      if (stat(s.spec.path.c_str(), &st) == 0 && IsDemoFileName(Basename(s.spec.path)) &&
          (s.finalSize < 0 || static_cast<int64_t>(st.st_size) == s.finalSize)) {
        if (unlink(s.spec.path.c_str()) == 0) Log(0, "fleet: demo " + s.file + ": local copy deleted (the platform has it)");
      }
      s.error = "deleted";
      dirty_ = true;
    }
    return;
  }
  if (s.phase == Phase::Refused || s.phase == Phase::Failed) return;

  struct stat st {};
  if (stat(s.spec.path.c_str(), &st) != 0) {
    const bool vanished = s.lastSize >= 0;
    if (vanished || (!s.recording && nowMs - s.stoppedAtMs > 60000)) {
      s.phase = Phase::Failed;
      s.error = vanished ? "file_vanished" : "file_not_found";
      s.doneAtMs = nowMs;
      dirty_ = true;
      Log(1, "fleet: demo " + s.file + ": " + s.error + "; not streamed");
    }
    return;
  }
  const int64_t size = static_cast<int64_t>(st.st_size);
  const uint64_t ino = static_cast<uint64_t>(st.st_ino);
  if ((s.inode != 0 && ino != s.inode) || size < s.lastSize || (s.finalSize < 0 && size < s.sent)) {
    Log(1, "fleet: demo " + s.file + ": the file was replaced or truncated; streaming it again from the start");
    s.restart = true;
    s.phase = Phase::Announce;
    s.announcedMs = 0;
    s.announceTries = 0;
    s.sent = s.acked = 0;
    s.fps.clear();
    s.rewrites.clear();
    s.finalSize = -1;
    s.sha256.clear();
    s.endSentMs = 0;
    dirty_ = true;
  }
  s.inode = ino;
  if (size != s.lastSize) {
    s.lastSize = size;
    s.sizeSinceMs = nowMs;
  }
  if (size > cfg_.maxDemoBytes) {
    s.phase = Phase::Refused;
    s.error = "too_large";
    s.doneAtMs = nowMs;
    dirty_ = true;
    Log(1, "fleet: demo " + s.file + ": larger than " + std::to_string(cfg_.maxDemoBytes) + " bytes; kept locally");
    return;
  }
  if (s.recording && nowMs - s.sizeSinceMs >= cfg_.staleRecordingMs) {
    s.recording = false;  // the recording ended without an End() (server restart, plugin crash)
    s.stoppedAtMs = nowMs;
    dirty_ = true;
  }
  if (!link.online || !link.online()) return;

  const uint32_t session = link.session ? link.session() : 0;
  if (s.phase == Phase::Stream && s.session != session) {
    s.phase = Phase::Announce;  // new session: ask the platform where to resume
    s.announcedMs = 0;
    s.announceTries = 0;
  }
  // Bytes in flight and the platform silent: ask again where it is.
  if (s.phase == Phase::Stream && s.sent > s.acked && nowMs - s.lastAckMs > cfg_.ackTimeoutMs) {
    Log(1, "fleet: demo " + s.file + ": no demo.ack for " + std::to_string(cfg_.ackTimeoutMs / 1000) + " s; resuming");
    s.phase = Phase::Announce;
    s.announcedMs = 0;
  }
  if (s.phase == Phase::Announce) {
    const int64_t backoff =
        s.announceTries == 0 ? 0 : std::min<int64_t>(cfg_.announceBackoffMaxMs, 5000ll << std::min(s.announceTries - 1, 12));
    if (s.announcedMs != 0 && nowMs - s.announcedMs < backoff) return;
    json::Value p = json::Value::Object();
    p.Set("demo_id", json::Value::Str(s.id));
    p.Set("match_id", json::Value::Str(s.spec.matchId));
    p.Set("map_number", json::Value::Int(s.spec.mapNumber));
    p.Set("file", json::Value::Str(s.file));
    p.Set("started_at", json::Value::Int(s.spec.startedAtMs));
    p.Set("chunk_size", json::Value::Int(s.chunk));
    p.Set("recording", json::Value::Bool(s.recording || s.finalSize < 0));
    if (s.restart) p.Set("restart", json::Value::Bool(true));
    if (link.send && link.send("demo.begin", json::Dump(p), s.spec.epoch)) {
      s.announcedMs = nowMs;
      ++s.announceTries;
    }
    return;
  }

  // Stream. The final pass once the recording stopped and the file stopped growing.
  if (!s.recording && s.finalSize < 0 && nowMs - s.stoppedAtMs >= cfg_.settleMs && nowMs - s.sizeSinceMs >= cfg_.settleMs) {
    if (!FinalPass(s)) return;
    dirty_ = true;
  }
  const int64_t limit = s.finalSize >= 0 ? s.finalSize : size;
  auto sendChunk = [&](int64_t off, int64_t len) {
    std::string data;
    if (!ReadRange(s.spec.path, off, len, &data)) return false;
    json::Value p = json::Value::Object();
    p.Set("demo_id", json::Value::Str(s.id));
    p.Set("offset", json::Value::Int(off));
    p.Set("size", json::Value::Int(len));
    p.Set("data", json::Value::Str(Base64Encode(data)));
    if (!link.sendBulk || !link.sendBulk("demo.chunk", json::Dump(p), s.spec.epoch)) return false;
    if (len == s.chunk) {
      const size_t idx = static_cast<size_t>(off / s.chunk);
      if (s.fps.size() <= idx) s.fps.resize(idx + 1, 0);
      s.fps[idx] = Fnv1a64(data.data(), data.size());
    }
    dirty_ = true;
    return true;
  };
  int budget = 16;  // chunks per tick
  while (s.sent < limit && budget-- > 0) {
    const int64_t len = std::min(s.chunk, limit - s.sent);
    if (s.finalSize < 0 && len < s.chunk) break;  // while recording: full chunks only
    if (s.sent > s.acked && s.sent - s.acked + len > cfg_.windowBytes) break;
    if (s.sent == s.acked) s.lastAckMs = nowMs;  // the ack timeout counts from the first unacked byte
    if (!sendChunk(s.sent, len)) break;
    s.sent += len;
  }
  if (s.finalSize < 0 || s.sent < s.finalSize) return;
  while (!s.rewrites.empty() && budget-- > 0) {
    const int64_t off = s.rewrites.front();
    if (!sendChunk(off, std::min(s.chunk, s.finalSize - off))) return;
    s.rewrites.erase(s.rewrites.begin());
  }
  if (!s.rewrites.empty()) return;
  // demo.end on the bulk lane too, so it arrives after every chunk sent before it.
  if (s.endSentMs == 0 || nowMs - s.endSentMs >= cfg_.endRetryMs) {
    json::Value p = json::Value::Object();
    p.Set("demo_id", json::Value::Str(s.id));
    p.Set("size", json::Value::Int(s.finalSize));
    p.Set("sha256", json::Value::Str(s.sha256));
    if (link.sendBulk && link.sendBulk("demo.end", json::Dump(p), s.spec.epoch)) s.endSentMs = nowMs;
  }
}

void Streams::Tick(const Link& link, int64_t nowMs) {
  std::deque<Cmd> q;
  {
    std::lock_guard<std::mutex> lk(qmu_);
    q.swap(queue_);
  }
  for (const auto& c : q) Apply(c, link, nowMs);
  for (auto& s : streams_) Work(s, link, nowMs);
  const size_t before = streams_.size();
  streams_.erase(std::remove_if(streams_.begin(), streams_.end(),
                                [&](const Stream& s) {
                                  if (s.phase == Phase::Done) return cfg_.keepAfterStoredMs <= 0 || s.error == "deleted";
                                  if (s.phase == Phase::Refused || s.phase == Phase::Failed) {
                                    return nowMs - s.doneAtMs >= cfg_.forgetFailedMs;
                                  }
                                  return false;
                                }),
                 streams_.end());
  if (streams_.size() != before) dirty_ = true;
  if (dirty_ && nowMs - lastSaveMs_ >= 2000) Save(nowMs);
  std::lock_guard<std::mutex> lk(qmu_);
  published_ = streams_;
}

std::vector<Stream> Streams::Snapshot() const {
  std::lock_guard<std::mutex> lk(qmu_);
  return published_;
}

size_t Streams::Unfinished() const {
  std::lock_guard<std::mutex> lk(qmu_);
  size_t n = 0;
  for (const auto& s : published_) n += (s.phase == Phase::Announce || s.phase == Phase::Stream) ? 1 : 0;
  for (const auto& c : queue_) n += c.kind == Cmd::BeginK ? 1 : 0;
  return n;
}

std::vector<std::string> Streams::StatusLines() const {
  std::vector<std::string> out;
  for (const auto& s : Snapshot()) {
    std::string l = "fleet: demo " + s.file + " (map " + std::to_string(s.spec.mapNumber) + " of " + s.spec.matchId +
                    "): " + PhaseName(s.phase) + (s.recording ? ", recording" : "") + ", sent " + std::to_string(s.sent) +
                    ", platform has " + std::to_string(s.acked);
    if (s.finalSize >= 0) l += " of " + std::to_string(s.finalSize);
    if (!s.error.empty()) l += " (" + s.error + ")";
    out.push_back(l);
  }
  return out;
}

void Streams::Save(int64_t nowMs) {
  lastSaveMs_ = nowMs;
  dirty_ = false;
  if (statePath_.empty()) return;
  json::Value arr = json::Value::Array();
  for (const auto& s : streams_) {
    json::Value o = json::Value::Object();
    o.Set("demo_id", json::Value::Str(s.id));
    o.Set("match_id", json::Value::Str(s.spec.matchId));
    o.Set("epoch", json::Value::Int(s.spec.epoch));
    o.Set("map_number", json::Value::Int(s.spec.mapNumber));
    o.Set("path", json::Value::Str(s.spec.path));
    o.Set("started_at", json::Value::Int(s.spec.startedAtMs));
    o.Set("chunk", json::Value::Int(s.chunk));
    o.Set("phase", json::Value::Str(PhaseName(s.phase)));
    o.Set("recording", json::Value::Bool(s.recording));
    o.Set("sent", json::Value::Int(s.sent));
    o.Set("acked", json::Value::Int(s.acked));
    std::string fps;
    for (uint64_t f : s.fps) fps += Hex64(f);
    o.Set("fps", json::Value::Str(fps));
    json::Value rw = json::Value::Array();
    for (int64_t r : s.rewrites) rw.Push(json::Value::Int(r));
    o.Set("rewrites", std::move(rw));
    o.Set("final_size", json::Value::Int(s.finalSize));
    o.Set("sha256", json::Value::Str(s.sha256));
    o.Set("restart", json::Value::Bool(s.restart));
    o.Set("checksum_failures", json::Value::Int(s.checksumFailures));
    o.Set("done_at", json::Value::Int(s.doneAtMs));
    o.Set("error", json::Value::Str(s.error));
    arr.Push(std::move(o));
  }
  json::Value root = json::Value::Object();
  root.Set("v", json::Value::Int(1));
  root.Set("streams", std::move(arr));
  std::string err;
  if (!WriteFileAtomic(statePath_, json::Dump(root) + "\n", 0600, &err)) Log(1, "fleet: could not save demo streams: " + err);
}

bool Streams::Load(int64_t nowMs) {
  if (statePath_.empty()) return false;
  std::string text;
  json::Value root;
  if (!ReadFile(statePath_, &text, 64u << 20) || !json::Parse(text, &root) || !root.IsObj()) return false;
  const json::Value* arr = root.Get("streams");
  if (!arr || !arr->IsArr()) return false;
  auto str = [](const json::Value& o, const char* k) { return o.Get(k) ? o.Get(k)->AsStr() : std::string(); };
  auto num = [](const json::Value& o, const char* k, int64_t d) { return o.Get(k) ? o.Get(k)->AsInt(d) : d; };
  streams_.clear();
  for (const auto& o : arr->a) {
    if (!o.IsObj()) continue;
    Stream s;
    s.id = str(o, "demo_id");
    s.spec.matchId = str(o, "match_id");
    s.spec.epoch = num(o, "epoch", 0);
    s.spec.mapNumber = static_cast<int>(num(o, "map_number", 1));
    s.spec.path = str(o, "path");
    s.spec.startedAtMs = num(o, "started_at", 0);
    s.file = Basename(s.spec.path);
    if (s.id.empty() || !IsDemoFileName(s.file)) continue;
    s.chunk = std::max(kChunkMin, std::min(kChunkMax, num(o, "chunk", kChunkDefault)));
    s.phase = PhaseFromName(str(o, "phase"));
    if (s.phase == Phase::Stream) s.phase = Phase::Announce;  // a new session: ask where to resume
    s.recording = o.Get("recording") && o.Get("recording")->AsBool();
    s.stoppedAtMs = nowMs;
    s.sizeSinceMs = nowMs;
    s.sent = num(o, "sent", 0);
    s.acked = num(o, "acked", 0);
    const std::string fps = str(o, "fps");
    for (size_t i = 0; i + 16 <= fps.size(); i += 16) {
      s.fps.push_back(std::strtoull(fps.substr(i, 16).c_str(), nullptr, 16));
    }
    if (const json::Value* rw = o.Get("rewrites"); rw && rw->IsArr()) {
      for (const auto& r : rw->a) s.rewrites.push_back(r.AsInt(0));
    }
    s.finalSize = num(o, "final_size", -1);
    s.sha256 = str(o, "sha256");
    s.restart = o.Get("restart") && o.Get("restart")->AsBool();
    s.checksumFailures = static_cast<int>(num(o, "checksum_failures", 0));
    s.doneAtMs = num(o, "done_at", 0);
    s.error = str(o, "error");
    streams_.push_back(std::move(s));
  }
  {
    std::lock_guard<std::mutex> lk(qmu_);
    published_ = streams_;
  }
  if (!streams_.empty()) Log(0, "fleet: " + std::to_string(streams_.size()) + " demo stream(s) restored");
  return !streams_.empty();
}

}  // namespace fleet::demo
