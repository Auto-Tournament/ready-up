#include "fleet_proto.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace fleet {

int64_t NowMs() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int64_t MonotonicMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool RandomBytes(void* out, size_t n) {
  auto* p = static_cast<unsigned char*>(out);
  size_t got = 0;
  while (got < n) {
    const ssize_t r = getrandom(p + got, n - got, 0);
    if (r > 0) {
      got += static_cast<size_t>(r);
      continue;
    }
    if (r < 0 && errno == EINTR) continue;
    break;
  }
  if (got == n) return true;
  const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  while (got < n) {
    const ssize_t r = read(fd, p + got, n - got);
    if (r <= 0) {
      if (r < 0 && errno == EINTR) continue;
      break;
    }
    got += static_cast<size_t>(r);
  }
  close(fd);
  return got == n;
}

std::string RandomHex(size_t bytes) {
  std::string raw(bytes, '\0');
  if (!RandomBytes(raw.data(), bytes)) {
    // Never expected; still produce something unique-ish rather than all zeros.
    for (size_t i = 0; i < bytes; ++i) raw[i] = static_cast<char>((MonotonicMs() >> (i % 8)) ^ (i * 131));
  }
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (unsigned char c : raw) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 15]);
  }
  return out;
}

namespace {
const char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
}

std::string NewUlid(int64_t ms) {
  unsigned char rnd[10];
  if (!RandomBytes(rnd, sizeof(rnd))) {
    const std::string h = RandomHex(10);
    std::memcpy(rnd, h.data(), sizeof(rnd));
  }
  // 128 bits: 48-bit time + 80 random, encoded as 26 base32 chars (first char holds 3 bits).
  unsigned char b[16];
  const uint64_t t = static_cast<uint64_t>(ms < 0 ? 0 : ms) & 0xFFFFFFFFFFFFull;
  for (int i = 0; i < 6; ++i) b[i] = static_cast<unsigned char>(t >> (8 * (5 - i)));
  std::memcpy(b + 6, rnd, 10);
  std::string out(26, '0');
  // Treat b as a big-endian 128-bit number and emit 5 bits at a time from the top (130 bits
  // with two leading zero bits).
  for (int c = 0; c < 26; ++c) {
    const int bitPos = c * 5 - 2;  // position of this char's first bit in the 128-bit number
    int v = 0;
    for (int k = 0; k < 5; ++k) {
      const int bit = bitPos + k;
      int bv = 0;
      if (bit >= 0) bv = (b[bit / 8] >> (7 - bit % 8)) & 1;
      v = (v << 1) | bv;
    }
    out[c] = kCrockford[v];
  }
  return out;
}

bool IsUlid(std::string_view s) {
  if (s.size() != 26) return false;
  for (char c : s) {
    if (!std::strchr(kCrockford, c) || c == '\0') return false;
  }
  return s[0] <= '7';
}

bool IsEphemeralType(std::string_view t) {
  return t == "ping" || t == "pong" || t == "ack" || t == "hello" || t == "welcome" || t == "error" ||
         t == "state.request" || t == "state.snapshot";
}

bool IsCriticalType(std::string_view t) {
  return t == "event.round_end" || t == "event.backup" || t == "event.map_result" || t == "event.series_end" ||
         t == "event.demo" || t == "cmd.result" || t == "skins.stattrak" || t == "auth.rotated" ||
         t == "event.rounds_voided" || t == "event.match_restored" || t == "event.forfeit";
}

bool IsValidType(std::string_view t) {
  if (t.empty() || t.size() > 64) return false;
  // Dot-separated segments, each [a-z][a-z0-9_]*.
  bool segStart = true;
  for (const char c : t) {
    if (c == '.') {
      if (segStart) return false;
      segStart = true;
      continue;
    }
    const bool lower = c >= 'a' && c <= 'z';
    const bool rest = (c >= '0' && c <= '9') || c == '_';
    if (!(lower || (!segStart && rest))) return false;
    segStart = false;
  }
  return !segStart;
}

namespace {
void AppendHead(const Envelope& e, std::string& out) {
  out += "{\"v\":";
  out += std::to_string(e.v);
  out += ",\"type\":";
  json::AppendQuoted(out, e.type);
  out += ",\"id\":";
  json::AppendQuoted(out, e.id);
  if (e.seq > 0) {
    out += ",\"seq\":";
    out += std::to_string(e.seq);
  }
  if (e.ack >= 0) {
    out += ",\"ack\":";
    out += std::to_string(e.ack);
  }
  out += ",\"ts\":";
  out += std::to_string(e.ts);
  out += ",\"ref\":";
  if (e.ref.empty()) out += "null";
  else json::AppendQuoted(out, e.ref);
  if (e.epoch > 0) {
    out += ",\"epoch\":";
    out += std::to_string(e.epoch);
  }
  out += ",\"payload\":";
}
}  // namespace

std::string Encode(const Envelope& e) {
  std::string out;
  AppendHead(e, out);
  json::DumpTo(e.payload.IsObj() ? e.payload : json::Value::Object(), out);
  out.push_back('}');
  return out;
}

std::string EncodeRaw(const Envelope& e, std::string_view payloadJson) {
  std::string out;
  out.reserve(payloadJson.size() + 160);
  AppendHead(e, out);
  out.append(payloadJson.empty() ? std::string_view("{}") : payloadJson);
  out.push_back('}');
  return out;
}

bool Decode(std::string_view text, Envelope* out, std::string* err) {
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  if (text.size() > kMaxFrameBytes) return fail("frame too large");
  json::Value v;
  std::string perr;
  if (!json::Parse(text, &v, &perr)) return fail("invalid JSON: " + perr);
  if (!v.IsObj()) return fail("envelope is not an object");
  Envelope e;
  const json::Value* ver = v.Get("v");
  if (!ver || ver->t != json::Value::T::Int) return fail("missing v");
  e.v = static_cast<int>(ver->i);
  const json::Value* type = v.Get("type");
  if (!type || !type->IsStr() || !IsValidType(type->s)) return fail("missing or invalid type");
  e.type = type->s;
  if (const json::Value* id = v.Get("id"); id && id->IsStr()) e.id = id->s;
  if (const json::Value* seq = v.Get("seq"); seq && !seq->IsNull()) {
    if (seq->t != json::Value::T::Int || seq->i < 1) return fail("invalid seq");
    e.seq = seq->i;
  }
  if (const json::Value* ack = v.Get("ack"); ack && !ack->IsNull()) {
    if (ack->t != json::Value::T::Int || ack->i < 0) return fail("invalid ack");
    e.ack = ack->i;
  }
  if (const json::Value* ts = v.Get("ts")) e.ts = ts->AsInt(0);
  if (const json::Value* ref = v.Get("ref"); ref && ref->IsStr()) e.ref = ref->s;
  if (const json::Value* ep = v.Get("epoch"); ep && !ep->IsNull()) e.epoch = ep->AsInt(0);
  if (json::Value* p = v.Get("payload")) {
    if (!p->IsObj()) return fail("payload is not an object");
    e.payload = std::move(*p);
  } else {
    return fail("missing payload");
  }
  if (out) *out = std::move(e);
  return true;
}

// ---- backoff -------------------------------------------------------------------------------

Backoff::Backoff(BackoffPolicy p, uint64_t seed) : p_(p) {
  if (seed == 0) {
    uint64_t s = 0;
    if (!RandomBytes(&s, sizeof(s))) s = static_cast<uint64_t>(MonotonicMs());
    seed = s;
  }
  rng_.seed(seed);
}

int64_t Backoff::Ceiling(int64_t capOverride, int64_t baseOverride) const {
  const int64_t base = baseOverride > 0 ? baseOverride : p_.baseMs;
  const int64_t cap = capOverride > 0 ? capOverride : p_.capMs;
  int64_t v = base;
  for (int i = 0; i < attempt_ && v < cap; ++i) v *= 2;
  return std::min(v, cap);
}

int64_t Backoff::Next(int64_t capOverride, int64_t baseOverride) {
  const int64_t ceil = Ceiling(capOverride, baseOverride);
  if (attempt_ < 62) ++attempt_;
  if (ceil <= 0) return 0;
  std::uniform_int_distribution<int64_t> d(0, ceil);
  return d(rng_);
}

void Backoff::OnSessionEnded(int64_t durationMs) {
  if (durationMs >= p_.resetAfterMs) attempt_ = 0;
}

// ---- close codes ---------------------------------------------------------------------------

namespace {
int64_t ParseRetryAfterMs(std::string_view reason) {
  // Accept {"retry_after_ms":1234}, "retry_after_ms=1234" or a bare number.
  const size_t k = reason.find("retry_after_ms");
  size_t i = k == std::string_view::npos ? 0 : k + 14;
  while (i < reason.size() && !std::isdigit(static_cast<unsigned char>(reason[i]))) ++i;
  int64_t v = 0;
  bool any = false;
  while (i < reason.size() && std::isdigit(static_cast<unsigned char>(reason[i])) && v < 86400000) {
    v = v * 10 + (reason[i] - '0');
    any = true;
    ++i;
  }
  return any ? v : -1;
}
}  // namespace

CloseAction ClassifyClose(int code, std::string_view reason) {
  CloseAction a;
  switch (code) {
    case 1000: a.what = "closed normally"; break;
    case 1001: a.what = "platform going away"; break;
    case 4400: a.what = "protocol error"; break;
    case 4401:
    case 4403:
      a.what = code == 4401 ? "credentials rejected (4401)" : "credentials revoked (4403)";
      a.rejected = true;
      a.capMs = 10 * 60 * 1000;
      break;
    case 4409:
      a.what = "replaced by a newer session (4409)";
      a.fixedDelayMs = 30000;
      break;
    case 4426:
      a.what = "protocol version unsupported (4426): update Ready Up";
      a.versionUnsupported = true;
      a.capMs = 10 * 60 * 1000;
      break;
    case 4429: {
      a.what = "rate limited (4429)";
      const int64_t ms = ParseRetryAfterMs(reason);
      a.fixedDelayMs = ms >= 0 ? ms : 30000;
      break;
    }
    case 4503:
      a.what = "platform draining (4503)";
      a.baseMs = 2000;
      break;
    default: {
      char buf[48];
      std::snprintf(buf, sizeof(buf), "closed with code %d", code);
      a.what = buf;
    }
  }
  if (!reason.empty() && code != 4429) {
    a.what += ": ";
    a.what += Redact(reason.substr(0, 120));
  }
  return a;
}

CloseAction ClassifyHttpStatus(long status, int64_t retryAfterMs) {
  CloseAction a;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "HTTP %ld", status);
  a.what = buf;
  if (status == 401 || status == 403) {
    a.rejected = true;
    a.capMs = 10 * 60 * 1000;
    a.what += " (credentials rejected)";
  } else if (status == 426) {
    a.versionUnsupported = true;
    a.capMs = 10 * 60 * 1000;
    a.what += " (protocol version unsupported)";
  } else if (status == 429) {
    a.fixedDelayMs = retryAfterMs > 0 ? retryAfterMs : 30000;
    a.what += " (rate limited)";
  } else if (status == 503) {
    a.baseMs = 2000;
  }
  return a;
}

// ---- redaction -----------------------------------------------------------------------------

std::string Redact(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  auto tokenChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '=' || c == '+' ||
           c == '/';
  };
  size_t i = 0;
  while (i < s.size()) {
    bool matched = false;
    for (const char* pfx : {"rus_", "rfk_", "rhs_", "rst_", "RUE-", "Bearer "}) {
      const size_t n = std::strlen(pfx);
      const bool bearer = pfx[0] == 'B';
      const bool boundary = i == 0 || !(std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_');
      if (s.compare(i, n, pfx) == 0 && (bearer || boundary)) {
        size_t j = i + n;
        while (j < s.size() && tokenChar(s[j])) ++j;
        out.append(pfx, n);
        out += "***";
        i = j;
        matched = true;
        break;
      }
    }
    if (!matched) out.push_back(s[i++]);
  }
  return out;
}

// ---- inbound tracking ----------------------------------------------------------------------

void RxTracker::Reset(int64_t processedSeq) {
  received_ = processed_ = std::max<int64_t>(0, processedSeq);
  lastAckSent_ = -1;
  firstUnackedAtMs_ = -1;
  pending_.clear();
}

void RxTracker::RememberId(const std::string& id) {
  if (id.empty()) return;
  if (ids_.insert(id).second) {
    idOrder_.push_back(id);
    if (idOrder_.size() > kDedupeWindow) {
      ids_.erase(idOrder_.front());
      idOrder_.pop_front();
    }
  }
}

RxTracker::Verdict RxTracker::OnReliable(int64_t seq, const std::string& id) {
  if (seq <= received_) return Verdict::Duplicate;
  if (!id.empty() && ids_.count(id)) return Verdict::Duplicate;
  if (seq != received_ + 1) return Verdict::OutOfOrder;
  received_ = seq;
  pending_[seq] = false;
  RememberId(id);
  return Verdict::Accept;
}

bool RxTracker::OnEphemeralId(const std::string& id) {
  if (id.empty()) return true;
  if (ids_.count(id)) return false;
  RememberId(id);
  return true;
}

bool RxTracker::MarkDone(int64_t seq, int64_t nowMs) {
  auto it = pending_.find(seq);
  if (it == pending_.end()) return false;
  it->second = true;
  const int64_t before = processed_;
  while (!pending_.empty() && pending_.begin()->second && pending_.begin()->first == processed_ + 1) {
    processed_ = pending_.begin()->first;
    pending_.erase(pending_.begin());
  }
  if (processed_ != before && firstUnackedAtMs_ < 0) firstUnackedAtMs_ = nowMs;
  return processed_ != before;
}

bool RxTracker::AckDue(int64_t nowMs) const {
  if (processed_ <= lastAckSent_ || processed_ == 0) return false;
  if (processed_ - std::max<int64_t>(lastAckSent_, 0) >= 32) return true;
  return firstUnackedAtMs_ >= 0 && nowMs - firstUnackedAtMs_ >= 1000;
}

int64_t RxTracker::NextAckDueMs() const {
  if (processed_ <= lastAckSent_ || processed_ == 0 || firstUnackedAtMs_ < 0) return -1;
  return firstUnackedAtMs_ + 1000;
}

void RxTracker::OnAckSent(int64_t ackedSeq, int64_t nowMs) {
  (void)nowMs;
  lastAckSent_ = std::max(lastAckSent_, ackedSeq);
  if (lastAckSent_ >= processed_) firstUnackedAtMs_ = -1;
}

// ---- URLs ----------------------------------------------------------------------------------

std::string JoinUrl(const std::string& base, const std::string& path, bool ws) {
  std::string b = base;
  while (!b.empty() && b.back() == '/') b.pop_back();
  if (ws) {
    if (b.rfind("https://", 0) == 0) b = "wss://" + b.substr(8);
    else if (b.rfind("http://", 0) == 0) b = "ws://" + b.substr(7);
  }
  return b + path;
}

namespace {

std::string SchemeOf(const std::string& url) {
  const size_t p = url.find("://");
  if (p == std::string::npos) return {};
  std::string s = url.substr(0, p);
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string HostOf(const std::string& url) {
  size_t p = url.find("://");
  if (p == std::string::npos) return {};
  p += 3;
  size_t end = url.find_first_of("/?#", p);
  std::string auth = url.substr(p, end == std::string::npos ? std::string::npos : end - p);
  const size_t at = auth.rfind('@');
  if (at != std::string::npos) auth = auth.substr(at + 1);
  if (!auth.empty() && auth[0] == '[') {
    const size_t rb = auth.find(']');
    return rb == std::string::npos ? std::string() : auth.substr(1, rb - 1);
  }
  const size_t colon = auth.find(':');
  if (colon != std::string::npos) auth = auth.substr(0, colon);
  for (auto& c : auth) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return auth;
}

}  // namespace

bool IsPrivateHostUrl(const std::string& url) {
  const std::string h = HostOf(url);
  if (h == "localhost" || (h.size() > 10 && h.compare(h.size() - 10, 10, ".localhost") == 0)) return true;
  in_addr a4{};
  if (inet_pton(AF_INET, h.c_str(), &a4) == 1) {
    const uint32_t ip = ntohl(a4.s_addr);
    return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == 0xAC1 /* 172.16/12 */ ||
           (ip >> 16) == 0xC0A8 /* 192.168/16 */ || (ip >> 16) == 0xA9FE /* 169.254/16 */;
  }
  in6_addr a6{};
  if (inet_pton(AF_INET6, h.c_str(), &a6) == 1) {
    static const unsigned char kLoop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (std::memcmp(a6.s6_addr, kLoop, 16) == 0) return true;
    if ((a6.s6_addr[0] & 0xFE) == 0xFC) return true;                                // fc00::/7
    if (a6.s6_addr[0] == 0xFE && (a6.s6_addr[1] & 0xC0) == 0x80) return true;       // fe80::/10
  }
  return false;
}

std::string CheckUrlAllowed(const std::string& url, bool insecureDev) {
  const std::string s = SchemeOf(url);
  if (s == "https" || s == "wss") return {};
  if (s == "http" || s == "ws") {
    if (!insecureDev) return "plain " + s + ":// needs insecure_dev 1 (and a loopback/private host)";
    if (!IsPrivateHostUrl(url)) return "plain " + s + ":// is only allowed to loopback or RFC 1918 hosts";
    return {};
  }
  return "unsupported URL scheme (want https:// or wss://)";
}


}  // namespace fleet
