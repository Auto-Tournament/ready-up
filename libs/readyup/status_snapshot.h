#pragma once
// Local status endpoint (docs/FLEET.md §17): the data side, engine-free.
//
//  - Json: a tiny JSON value (object keys keep insertion order) with a serializer and an
//    RFC 7386 merge-patch diff, so `/stream` can send `{rev, patch}` deltas.
//  - StatusInputs: what the game thread collects (status_feed.cpp) a few times a second.
//  - Hub: the hand-off. The game thread only Submit()s a shared_ptr (a mutex held for the
//    pointer swap). The HTTP thread Process()es it: serializes, diffs against the previous
//    inputs, bumps `rev` and appends `patch` / `status` events to a bounded ring. Every other
//    Hub member is HTTP-thread only (single consumer), so requests never touch game state.
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace readyup::status {

class Json {
 public:
  enum class Type { Null, Bool, Int, Double, String, Object, Array };

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : t_(Type::Bool), i_(b ? 1 : 0) {}
  Json(int v) : t_(Type::Int), i_(v) {}
  Json(long v) : t_(Type::Int), i_(v) {}
  Json(long long v) : t_(Type::Int), i_(v) {}
  Json(unsigned v) : t_(Type::Int), i_(static_cast<long long>(v)) {}
  Json(unsigned long v) : t_(Type::Int), i_(static_cast<long long>(v)) {}
  Json(unsigned long long v) : t_(Type::Int), i_(static_cast<long long>(v)) {}
  Json(double v) : t_(Type::Double), d_(v) {}
  Json(const char* s) : t_(s ? Type::String : Type::Null), s_(s ? s : "") {}
  Json(std::string s) : t_(Type::String), s_(std::move(s)) {}

  static Json Object() {
    Json j;
    j.t_ = Type::Object;
    return j;
  }
  static Json Array() {
    Json j;
    j.t_ = Type::Array;
    return j;
  }

  Type type() const { return t_; }
  bool IsNull() const { return t_ == Type::Null; }
  bool IsObject() const { return t_ == Type::Object; }

  // Object access; a null value becomes an empty object. Adds the key when missing.
  Json& operator[](const std::string& key);
  const Json* Find(const std::string& key) const;
  // Array append; a null value becomes an empty array.
  void Push(Json v);

  const std::vector<std::pair<std::string, Json>>& Members() const { return obj_; }
  const std::vector<Json>& Items() const { return arr_; }
  bool AsBool() const { return i_ != 0; }
  long long AsInt() const { return t_ == Type::Double ? static_cast<long long>(d_) : i_; }
  double AsDouble() const { return t_ == Type::Double ? d_ : static_cast<double>(i_); }
  const std::string& AsString() const { return s_; }

  std::string Dump() const;
  void DumpTo(std::string& out) const;

  // Parses JSON text (RFC 8259; object key order is kept, integers without a fraction or
  // exponent stay Int). False and *err on a syntax error or nesting deeper than 64.
  static bool Parse(const std::string& text, Json* out, std::string* err = nullptr);

  bool operator==(const Json& o) const;  // object key order does not matter
  bool operator!=(const Json& o) const { return !(*this == o); }

 private:
  Type t_ = Type::Null;
  long long i_ = 0;
  double d_ = 0.0;
  std::string s_;
  std::vector<std::pair<std::string, Json>> obj_;
  std::vector<Json> arr_;
};

void JsonEscapeTo(std::string& out, const std::string& s);

// RFC 7386 merge patch that turns `from` into `to`. Returns false when they are equal.
// Removed object keys become null; arrays and scalars are replaced whole.
bool MergeDiff(const Json& from, const Json& to, Json* patch);
// RFC 7386 apply (tests and consumers).
Json MergePatchApply(Json target, const Json& patch);

// One SSE message: `event: <event>\n[id: <id>\n]data: <line>\n...\n\n` (data split on '\n').
std::string SseFrame(const std::string& event, const std::string& id, const std::string& data);
inline std::string SseComment(const std::string& text) { return ": " + text + "\n\n"; }

// ---------------------------------------------------------------------------- inputs

struct StatusInputs {
  // Static-ish identity.
  std::string hostname;
  std::string server_id;  // empty = not enrolled
  int game_port = 0;

  Json versions;  // {core, plugin_api, plugins:{name:version}, cs2_build, cs2_patch}
  Json selftest;  // {pass, passed, total, failures[], ran_at} or null (never ran)
  Json platform;  // {mode, state, since, reconnects, spool_msgs, auto_pause_in_s?}
  Json summary;   // flat table-row fields
  Json state;     // MatchState (docs/FLEET.md §9.1) or null
  bool update_safe = true;

  // /health: false when the engine surface is disabled or the last selftest failed.
  bool healthy = true;
  std::string unhealthy_reason;
  // /selftest body (the last full report; empty = never ran).
  std::string selftest_report;
  // Extra Prometheus gauges from the game thread: name -> value (names are prefixed readyup_).
  std::vector<std::pair<std::string, double>> gauges;
};

struct StreamEvent {
  uint64_t seq = 0;  // Hub-internal, strictly increasing
  uint64_t rev = 0;  // patch events: the new rev (SSE id); 0 for status events
  std::string frame; // the complete SSE frame
};

class Hub {
 public:
  explicit Hub(size_t patchRing = 256);

  // Any thread (the game thread). Holds the mutex only for the pointer swap.
  void Submit(std::shared_ptr<const StatusInputs> in);
  uint64_t Submissions() const;

  // ---- HTTP thread only below --------------------------------------------------------
  // Picks up the latest submission, if any. Returns true if new stream events were appended
  // (or on the first submission).
  bool Process();

  bool HasData() const { return static_cast<bool>(cur_); }
  uint64_t Rev() const { return rev_; }
  uint64_t LastSeq() const { return lastSeq_; }
  std::shared_ptr<const StatusInputs> Current() const { return cur_; }

  // The /status body. `uptime_s` / `generated_at` (unix ms) are filled in per request.
  std::string StatusBody(long long uptimeS, long long nowMs) const;
  // `event: snapshot` frame for a new or resynced stream.
  std::string SnapshotFrame() const;
  // Frames with seq > afterSeq appended to *out; *lastSeq = newest seq. False when events
  // after `afterSeq` already fell out of the ring (send a fresh snapshot instead).
  bool FramesAfter(uint64_t afterSeq, std::string* out, uint64_t* lastSeq) const;
  // Last-Event-ID resume: the seq of the patch event with this rev, if still in the ring.
  bool SeqForRev(uint64_t rev, uint64_t* seq) const;
  size_t RingSize() const { return events_.size(); }

 private:
  void Append(uint64_t rev, std::string frame);

  mutable std::mutex mu_;
  std::shared_ptr<const StatusInputs> pending_;
  uint64_t submissions_ = 0;

  size_t cap_;
  std::shared_ptr<const StatusInputs> cur_;
  uint64_t rev_ = 0;
  uint64_t lastSeq_ = 0;
  uint64_t oldestDroppedSeq_ = 0;  // seqs <= this are gone
  std::deque<StreamEvent> events_;
  // Serialized pieces of cur_ (for /status and change detection).
  std::string versionsJ_, selftestJ_, platformJ_, summaryJ_, stateJ_;
};

}  // namespace readyup::status
