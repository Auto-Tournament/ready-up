// Demo streaming over the fleet link (docs/FLEET.md §12.2): while CS2 records a GOTV demo of a
// platform match, the growing .dem file is sent to the platform in chunks, and finished with its
// size and sha256. The local file is the buffer: the platform acknowledges how many bytes it has
// stored (demo.ack offset) and every resume (reconnect, plugin reload, server restart) continues
// from there, never from zero. The file is deleted only after the platform confirmed the whole
// file (demo.ack complete), and only after keepAfterStoredMs (0 = never).
//
// Without a fleet link nothing here runs: fleet.so only streams files the match plugin hands it
// for platform-assigned matches (fleet_iface.h demo_stream_begin), and never touches any other.
//
// Messages (schemas: protocol/v1/messages/demo.*.json):
//   server -> platform  demo.begin {demo_id, match_id, map_number, file, started_at, chunk_size,
//                                   recording, restart?}                        normal lane
//                       demo.chunk {demo_id, offset, size, data (base64)}       bulk lane (lowest)
//                       demo.end   {demo_id, size, sha256}                      bulk lane (after the chunks)
//   platform -> server  demo.ack   {demo_id, offset, complete?, error?: {code, message?}}
//
// Threads: Begin / End / OnAck may be called from any thread; they only queue. Tick (the
// streamer thread) does all file I/O and sends; Snapshot / Unfinished read what Tick published.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace fleet::demo {

// ---- helpers --------------------------------------------------------------------------------

class Sha256 {
 public:
  Sha256();
  void Update(const void* data, size_t len);
  std::string HexDigest();  // finishes; call once

 private:
  void Block(const unsigned char* p);
  uint32_t h_[8];
  unsigned char buf_[64];
  size_t bufLen_ = 0;
  uint64_t total_ = 0;
};
std::string Base64Encode(const std::string& data);
uint64_t Fnv1a64(const void* data, size_t len);
// A demo file name the platform may store: [A-Za-z0-9_.-], 5..200 bytes, ends in ".dem".
bool IsDemoFileName(const std::string& name);

// ---- streams --------------------------------------------------------------------------------

constexpr int64_t kChunkDefault = 128 * 1024;
constexpr int64_t kChunkMin = 4 * 1024;
constexpr int64_t kChunkMax = 512 * 1024;  // base64 + envelope stay under the 1 MiB frame limit

struct Config {
  int64_t chunkBytes = kChunkDefault;       // kChunkMin .. kChunkMax
  int64_t windowBytes = 1 << 20;            // bytes sent but not acked yet (at least one chunk)
  int64_t ackTimeoutMs = 15000;             // bytes in flight and no demo.ack this long -> demo.begin again
  int64_t announceBackoffMaxMs = 300000;    // demo.begin retries: 5 s, 10 s, 20 s ... up to this
  int64_t settleMs = 3000;                  // after the stop: size unchanged this long -> final pass
  int64_t staleRecordingMs = 300000;        // "recording" but no growth this long (crash / restart) -> finished
  int64_t endRetryMs = 10000;               // demo.end sent again until demo.ack complete
  int64_t keepAfterStoredMs = 24ll * 3600 * 1000;  // delete the local file this long after complete; 0 = keep
  int64_t forgetFailedMs = 7ll * 24 * 3600 * 1000; // refused / failed entries are dropped (file kept)
  int maxChecksumRetries = 3;
  int64_t maxDemoBytes = 4ll << 30;
};

enum class Phase {
  Announce,  // demo.begin sent (or due), waiting for the demo.ack that says where to resume
  Stream,    // chunks (while recording: full chunks only), then the final pass, the tail, demo.end
  Done,      // the platform stored the whole file (sha256 checked)
  Refused,   // the platform refused it (not_assigned, stale_epoch, too_large): kept locally
  Failed     // the file vanished, or checksum retries used up: kept locally
};
const char* PhaseName(Phase p);

struct Spec {
  std::string matchId;
  int64_t epoch = 0;
  int mapNumber = 1;
  std::string path;  // absolute path of the .dem (may not exist yet: tv_record runs next frame)
  int64_t startedAtMs = 0;
};

struct Stream {
  std::string id;  // demo_id (ULID)
  Spec spec;
  std::string file;  // basename of spec.path
  int64_t chunk = kChunkDefault;
  Phase phase = Phase::Announce;
  bool recording = true;
  int64_t stoppedAtMs = 0;
  int64_t sent = 0;                // next forward byte to send
  int64_t acked = 0;               // the platform's stored offset (last demo.ack)
  std::vector<uint64_t> fps;       // Fnv1a64 of full chunk i as sent (0 = unknown)
  std::vector<int64_t> rewrites;   // chunk offsets whose bytes changed after they were sent
  int64_t lastSize = -1;
  int64_t sizeSinceMs = 0;         // when lastSize was first seen
  uint64_t inode = 0;
  int64_t finalSize = -1;          // set by the final pass
  std::string sha256;              // of the whole final file
  bool restart = false;            // next demo.begin asks the platform to drop what it has
  uint32_t session = 0;            // link session the platform answered demo.begin in
  int64_t announcedMs = 0;
  int announceTries = 0;
  int64_t lastAckMs = 0;
  int64_t endSentMs = 0;
  int checksumFailures = 0;
  int64_t doneAtMs = 0;
  std::string error;               // last platform error code, or the local reason
};

// What Tick needs from the link (fleet_client.h Client). Called on the streamer thread.
struct Link {
  std::function<bool()> online;
  std::function<uint32_t()> session;  // changes with every new session (Client status.sessions)
  std::function<bool(const std::string& type, const std::string& payload, int64_t epoch)> send;
  std::function<bool(const std::string& type, const std::string& payload, int64_t epoch)> sendBulk;
};

class Streams {
 public:
  // statePath: where the streams are saved (JSON, 0600); "" = not saved (tests).
  Streams(Config cfg, std::string statePath, std::function<void(int, const std::string&)> log = {});

  // Any thread. A recording started: returns the new demo_id, "" when refused (bad file name).
  std::string Begin(const Spec& spec, int64_t nowMs);
  // Any thread. The recording of `path` stopped (tv_stoprecord queued after the GOTV flush).
  void End(const std::string& path, int64_t nowMs);
  // Any thread. A demo.ack payload from the platform.
  void OnAck(const std::string& payloadJson);

  // Streamer thread. Loads the saved streams (at plugin load). False when there are none.
  bool Load(int64_t nowMs);
  // Streamer thread: applies what was queued, reads the files, sends, deletes, saves.
  void Tick(const Link& link, int64_t nowMs);

  // Any thread: what the last Tick left (Begin / End not yet ticked are not in it).
  std::vector<Stream> Snapshot() const;
  // Streams the platform has not confirmed and still could (Announce / Stream).
  size_t Unfinished() const;
  std::vector<std::string> StatusLines() const;

 private:
  struct Cmd {
    enum Kind { BeginK, EndK, AckK } kind;
    Stream begin;      // BeginK
    std::string text;  // EndK: path; AckK: payload
    int64_t nowMs = 0;
  };
  void Apply(const Cmd& c, const Link& link, int64_t nowMs);
  void ApplyAck(const std::string& payload, const Link& link, int64_t nowMs);
  void Work(Stream& s, const Link& link, int64_t nowMs);
  bool FinalPass(Stream& s);
  void Save(int64_t nowMs);
  void Log(int level, const std::string& msg) const;

  Config cfg_;
  std::string statePath_;
  std::function<void(int, const std::string&)> log_;

  mutable std::mutex qmu_;  // guards queue_ and published_
  std::deque<Cmd> queue_;
  std::vector<Stream> published_;

  std::vector<Stream> streams_;  // streamer thread only
  bool dirty_ = false;
  int64_t lastSaveMs_ = 0;
};

}  // namespace fleet::demo
