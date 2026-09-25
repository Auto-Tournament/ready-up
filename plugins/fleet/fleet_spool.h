// The server's outbound reliable stream (docs/FLEET.md §6.4, §6.5), kept on disk until the
// platform acks it, so it survives reconnects and process restarts.
//
//   <dir>/meta.json    {"stream_id","next_seq","acked_seq","rx_seq","gap"}
//   <dir>/stream.log   one JSON record per line: {"seq","type","id","ts","epoch","crit","payload"}
//
// Bounded (default 50 000 messages / 64 MiB). When full, compactable messages are dropped
// oldest first; critical ones (§6.5) are kept (up to 2x the limits). Any drop sets `gap`,
// which makes the next connect start a new stream (the platform then answers `reset` and
// gets a state.snapshot).
//
// Not thread-safe: owned by the network thread.
#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>

namespace fleet {

struct SpoolRecord {
  int64_t seq = 0;
  std::string type;
  std::string id;
  std::string ref;  // id of the message this answers ("" = none)
  int64_t ts = 0;
  int64_t epoch = 0;
  bool critical = false;
  std::string payload;  // JSON object text

  uint64_t Bytes() const { return payload.size() + type.size() + id.size() + ref.size() + 96; }
};

class Spool {
 public:
  struct Limits {
    size_t maxMsgs = 50000;
    uint64_t maxBytes = 64ull << 20;
  };

  Spool() = default;
  ~Spool();
  Spool(const Spool&) = delete;
  Spool& operator=(const Spool&) = delete;

  // Loads (or creates) the spool in `dir`. `note` gets a human line about what was loaded.
  bool Open(const std::string& dir, Limits limits, std::string* err, std::string* note);
  void Close();
  bool IsOpen() const { return log_ != nullptr; }

  // Assigns the next seq and stores the message. Returns the seq, or 0 if it was refused
  // (spool full; `why` says so).
  int64_t Append(const std::string& type, const std::string& id, const std::string& ref, int64_t ts, int64_t epoch,
                 const std::string& payloadJson, std::string* why);

  // The platform acked everything <= seq.
  void AckUpTo(int64_t seq);

  // Starts a new stream if a gap was flagged: new stream id, retained messages renumbered from
  // 1. Returns true if it did (the resume is then a reset).
  bool RotateIfGap();

  const std::deque<SpoolRecord>& records() const { return recs_; }
  const std::string& streamId() const { return streamId_; }
  int64_t lastSeq() const { return nextSeq_ - 1; }
  int64_t ackedSeq() const { return ackedSeq_; }
  int64_t rxSeq() const { return rxSeq_; }
  bool gap() const { return gap_; }
  size_t count() const { return recs_.size(); }
  uint64_t bytes() const { return bytes_; }
  uint64_t dropped() const { return dropped_; }

  // Highest contiguous platform seq processed (what we ack); persisted with the meta.
  void SetRxSeq(int64_t seq);
  // Writes meta.json now, or at most once per second with MaybeFlush.
  bool FlushMeta();
  void MaybeFlush(int64_t nowMs);

 private:
  bool LoadMeta(std::string* note);
  bool LoadLog(std::string* note);
  bool Rewrite();
  bool AppendLine(const SpoolRecord& r);
  bool DropOneCompactable();

  std::string dir_;
  Limits limits_;
  FILE* log_ = nullptr;
  uint64_t logBytes_ = 0;
  std::deque<SpoolRecord> recs_;
  uint64_t bytes_ = 0;
  uint64_t dropped_ = 0;
  std::string streamId_;
  int64_t nextSeq_ = 1;
  int64_t ackedSeq_ = 0;
  int64_t rxSeq_ = 0;
  bool gap_ = false;
  bool metaDirty_ = false;
  int64_t lastFlushMs_ = 0;
};

}  // namespace fleet
