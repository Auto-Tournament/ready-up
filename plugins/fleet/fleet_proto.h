// Fleet protocol building blocks with no I/O (docs/FLEET.md §5, §6): the envelope, ULIDs,
// reconnect backoff, close-code policy, inbound seq/ack tracking and secret redaction.
// Everything here is unit-tested in tests/fleet_unit_test.cpp.
#pragma once

#include "fleet_json.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>

namespace fleet {

constexpr int kProtocolVersion = 1;
constexpr size_t kMaxFrameBytes = 1u << 20;  // §5: one text frame, max 1 MiB
constexpr size_t kDedupeWindow = 10000;      // §5: id dedupe window

int64_t NowMs();        // wall clock, unix ms
int64_t MonotonicMs();  // CLOCK_MONOTONIC

// Fills `n` bytes from the kernel CSPRNG (getrandom, /dev/urandom fallback). false on failure.
bool RandomBytes(void* out, size_t n);
std::string RandomHex(size_t bytes);

// ULID: 48-bit ms timestamp + 80 random bits, Crockford base32, 26 chars.
std::string NewUlid(int64_t ms);
bool IsUlid(std::string_view s);

// Message type classes.
bool IsEphemeralType(std::string_view type);  // ping, pong, ack, hello, welcome, error, state.request, ...
bool IsCriticalType(std::string_view type);   // §6.5 critical: never dropped from a full spool
bool IsValidType(std::string_view type);      // [a-z][a-z0-9_]*(\.[a-z0-9_]+)*, max 64

struct Envelope {
  int v = kProtocolVersion;
  std::string type;
  std::string id;
  int64_t seq = 0;   // 0 = absent (ephemeral)
  int64_t ack = -1;  // -1 = absent
  int64_t ts = 0;
  std::string ref;   // "" = null
  int64_t epoch = 0; // 0 = absent
  json::Value payload = json::Value::Object();
};

std::string Encode(const Envelope& e);
// Payload is spliced in verbatim (it must already be a valid JSON object text).
std::string EncodeRaw(const Envelope& e, std::string_view payloadJson);
bool Decode(std::string_view text, Envelope* out, std::string* err);

// §6.3: delay = min(cap, base * 2^attempt) with full jitter; reset after a session lasted
// resetAfterMs.
struct BackoffPolicy {
  int64_t baseMs = 1000;
  int64_t capMs = 30000;
  int64_t resetAfterMs = 60000;
};

class Backoff {
 public:
  explicit Backoff(BackoffPolicy p = {}, uint64_t seed = 0);
  // Next delay (uniform in [0, min(cap, base*2^attempt)]); bumps the attempt counter.
  // capOverride > 0 replaces the cap (4401/4403/4426 -> 10 min), baseOverride > 0 the base.
  int64_t Next(int64_t capOverride = 0, int64_t baseOverride = 0);
  // Upper bound Next() would use now (no jitter), for tests and status.
  int64_t Ceiling(int64_t capOverride = 0, int64_t baseOverride = 0) const;
  void Reset() { attempt_ = 0; }
  // Call when a session ends; resets the attempt counter if it lasted long enough.
  void OnSessionEnded(int64_t durationMs);
  int attempt() const { return attempt_; }
  const BackoffPolicy& policy() const { return p_; }

 private:
  BackoffPolicy p_;
  int attempt_ = 0;
  std::mt19937_64 rng_;
};

// What to do after a close (§6.3 close-code table, plus HTTP statuses of a refused upgrade).
struct CloseAction {
  int64_t fixedDelayMs = -1;  // >= 0: wait exactly this long (4409, 4429) instead of backoff
  int64_t capMs = 0;          // > 0: backoff cap override (10 min for 4401/4403/4426)
  int64_t baseMs = 0;         // > 0: backoff base override (4503: 2 s)
  bool rejected = false;      // credentials refused: log loudly, state "rejected"
  bool versionUnsupported = false;
  std::string what;           // human description for logs/status
};
CloseAction ClassifyClose(int code, std::string_view reason);
CloseAction ClassifyHttpStatus(long status, int64_t retryAfterMs);

// Replaces rus_/rfk_/rhs_/rst_ tokens, RUE- enrollment codes and "Bearer x" with a redacted form.
std::string Redact(std::string_view s);

// http(s) base URL + path, with "ws"/"wss" when `ws`.
std::string JoinUrl(const std::string& base, const std::string& path, bool ws);
// True if the URL's host is loopback, RFC 1918, link-local or "localhost".
bool IsPrivateHostUrl(const std::string& url);
// §4.4 scheme check. Empty = allowed, otherwise the reason.
std::string CheckUrlAllowed(const std::string& url, bool insecureDev);

// Inbound reliable stream (platform -> server). Tracks the highest contiguous seq received,
// the highest contiguous seq processed (what we ack), duplicates by seq and by id.
class RxTracker {
 public:
  enum class Verdict { Accept, Duplicate, OutOfOrder };

  void Reset(int64_t processedSeq);  // after loading persisted state
  // A reliable message arrived. Accept: seq == received+1 and id not seen.
  Verdict OnReliable(int64_t seq, const std::string& id);
  // Ephemeral messages are deduped by id only. true = new.
  bool OnEphemeralId(const std::string& id);
  // The handler for `seq` finished (any order); advances `processed` over the done prefix.
  // Returns true if `processed` advanced.
  bool MarkDone(int64_t seq, int64_t nowMs);
  int64_t received() const { return received_; }
  int64_t processed() const { return processed_; }
  size_t inFlight() const { return pending_.size(); }

  // Ack scheduling (§5: piggyback, or standalone at most 1 s / 32 messages after receipt).
  // true when a standalone ack should go out now.
  bool AckDue(int64_t nowMs) const;
  int64_t NextAckDueMs() const;  // -1 = nothing pending
  void OnAckSent(int64_t ackedSeq, int64_t nowMs);
  int64_t lastAckSent() const { return lastAckSent_; }

 private:
  void RememberId(const std::string& id);

  int64_t received_ = 0;
  int64_t processed_ = 0;
  int64_t lastAckSent_ = -1;
  int64_t firstUnackedAtMs_ = -1;
  std::map<int64_t, bool> pending_;  // received but not yet done-contiguous: seq -> done
  std::deque<std::string> idOrder_;
  std::unordered_set<std::string> ids_;
};

}  // namespace fleet
