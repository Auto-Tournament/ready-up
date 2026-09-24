#include "fleet_spool.h"

#include "fleet_json.h"
#include "fleet_proto.h"
#include "fleet_store.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace fleet {

namespace {
std::string RecordLine(const SpoolRecord& r) {
  std::string out;
  out.reserve(r.payload.size() + 160);
  out += "{\"seq\":";
  out += std::to_string(r.seq);
  out += ",\"type\":";
  json::AppendQuoted(out, r.type);
  out += ",\"id\":";
  json::AppendQuoted(out, r.id);
  if (!r.ref.empty()) {
    out += ",\"ref\":";
    json::AppendQuoted(out, r.ref);
  }
  out += ",\"ts\":";
  out += std::to_string(r.ts);
  out += ",\"epoch\":";
  out += std::to_string(r.epoch);
  out += r.critical ? ",\"crit\":true" : ",\"crit\":false";
  out += ",\"payload\":";
  out += r.payload;
  out += "}\n";
  return out;
}
}  // namespace

Spool::~Spool() { Close(); }

void Spool::Close() {
  if (log_) {
    if (metaDirty_) FlushMeta();
    std::fclose(log_);
    log_ = nullptr;
  }
}

bool Spool::Open(const std::string& dir, Limits limits, std::string* err, std::string* note) {
  Close();
  dir_ = dir;
  limits_ = limits;
  recs_.clear();
  bytes_ = 0;
  if (!MakeDirs(dir_, 0700, err)) return false;
  std::string n1, n2;
  const bool haveMeta = LoadMeta(&n1);
  LoadLog(&n2);
  if (!haveMeta) {
    streamId_ = NewUlid(NowMs());
    ackedSeq_ = 0;
    rxSeq_ = 0;
    nextSeq_ = recs_.empty() ? 1 : recs_.back().seq + 1;
    gap_ = !recs_.empty();  // messages without a known stream: start over as a reset
  }
  // Drop what the meta says was already acked.
  while (!recs_.empty() && recs_.front().seq <= ackedSeq_) {
    bytes_ -= recs_.front().Bytes();
    recs_.pop_front();
  }
  if (!recs_.empty() && recs_.back().seq >= nextSeq_) nextSeq_ = recs_.back().seq + 1;
  if (!Rewrite()) {
    if (err) *err = "cannot write " + dir_ + "/stream.log: " + std::strerror(errno);
    return false;
  }
  metaDirty_ = true;
  if (!FlushMeta()) {
    if (err) *err = "cannot write " + dir_ + "/meta.json";
    return false;
  }
  if (note) {
    *note = "stream " + streamId_ + ", " + std::to_string(recs_.size()) + " unacked, tx seq " +
            std::to_string(lastSeq()) + ", rx seq " + std::to_string(rxSeq_);
    if (!n1.empty()) *note += "; " + n1;
    if (!n2.empty()) *note += "; " + n2;
    if (gap_) *note += "; gap (next connect resets the stream)";
  }
  return true;
}

bool Spool::LoadMeta(std::string* note) {
  std::string text;
  if (!ReadFile(dir_ + "/meta.json", &text, 1u << 16)) return false;
  json::Value v;
  if (!json::Parse(text, &v) || !v.IsObj() || !v.Get("stream_id") || !v.Get("stream_id")->IsStr()) {
    *note = "meta.json corrupt, starting a new stream";
    return false;
  }
  streamId_ = v.Get("stream_id")->s;
  nextSeq_ = std::max<int64_t>(1, v.Get("next_seq") ? v.Get("next_seq")->AsInt(1) : 1);
  ackedSeq_ = std::max<int64_t>(0, v.Get("acked_seq") ? v.Get("acked_seq")->AsInt(0) : 0);
  rxSeq_ = std::max<int64_t>(0, v.Get("rx_seq") ? v.Get("rx_seq")->AsInt(0) : 0);
  gap_ = v.Get("gap") && v.Get("gap")->AsBool(false);
  return !streamId_.empty();
}

bool Spool::LoadLog(std::string* note) {
  std::string text;
  if (!ReadFile(dir_ + "/stream.log", &text, 1ull << 30)) return false;
  size_t pos = 0, bad = 0;
  int64_t prev = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    const std::string_view line(text.data() + pos, nl - pos);
    pos = nl + 1;
    if (line.empty()) continue;
    json::Value v;
    if (!json::Parse(line, &v) || !v.IsObj()) {
      ++bad;
      continue;
    }
    SpoolRecord r;
    r.seq = v.Get("seq") ? v.Get("seq")->AsInt(0) : 0;
    r.type = v.Get("type") ? v.Get("type")->AsStr() : "";
    r.id = v.Get("id") ? v.Get("id")->AsStr() : "";
    r.ref = v.Get("ref") ? v.Get("ref")->AsStr() : "";
    r.ts = v.Get("ts") ? v.Get("ts")->AsInt(0) : 0;
    r.epoch = v.Get("epoch") ? v.Get("epoch")->AsInt(0) : 0;
    r.critical = v.Get("crit") && v.Get("crit")->AsBool(false);
    const json::Value* p = v.Get("payload");
    if (r.seq <= prev || r.type.empty() || !p || !p->IsObj()) {
      ++bad;
      continue;
    }
    r.payload = json::Dump(*p);
    prev = r.seq;
    bytes_ += r.Bytes();
    recs_.push_back(std::move(r));
  }
  if (bad) {
    // A torn last line after a crash is expected; anything else is a hole in the stream.
    gap_ = true;
    *note = std::to_string(bad) + " unreadable spool record(s) skipped";
  }
  return true;
}

bool Spool::Rewrite() {
  if (log_) {
    std::fclose(log_);
    log_ = nullptr;
  }
  std::string data;
  for (const auto& r : recs_) data += RecordLine(r);
  std::string err;
  const std::string path = dir_ + "/stream.log";
  if (!WriteFileAtomic(path, data, 0600, &err)) return false;
  log_ = std::fopen(path.c_str(), "ab");
  logBytes_ = data.size();
  return log_ != nullptr;
}

bool Spool::AppendLine(const SpoolRecord& r) {
  if (!log_) return false;
  const std::string line = RecordLine(r);
  if (std::fwrite(line.data(), 1, line.size(), log_) != line.size()) return false;
  std::fflush(log_);
  logBytes_ += line.size();
  return true;
}

bool Spool::DropOneCompactable() {
  for (auto it = recs_.begin(); it != recs_.end(); ++it) {
    if (!it->critical) {
      bytes_ -= it->Bytes();
      recs_.erase(it);
      ++dropped_;
      gap_ = true;
      metaDirty_ = true;
      return true;
    }
  }
  return false;
}

int64_t Spool::Append(const std::string& type, const std::string& id, const std::string& ref, int64_t ts,
                      int64_t epoch, const std::string& payloadJson, std::string* why) {
  SpoolRecord r;
  r.type = type;
  r.id = id;
  r.ref = ref;
  r.ts = ts;
  r.epoch = epoch;
  r.critical = IsCriticalType(type);
  r.payload = payloadJson.empty() ? "{}" : payloadJson;
  const uint64_t add = r.Bytes();
  auto over = [&](double f) {
    return static_cast<double>(recs_.size() + 1) > static_cast<double>(limits_.maxMsgs) * f ||
           static_cast<double>(bytes_ + add) > static_cast<double>(limits_.maxBytes) * f;
  };
  bool droppedAny = false;
  while (over(1.0) && DropOneCompactable()) droppedAny = true;
  if (droppedAny) {
    // Make room for a while so a full spool is not rewritten on every message.
    while ((static_cast<double>(recs_.size()) > static_cast<double>(limits_.maxMsgs) * 0.9 ||
            static_cast<double>(bytes_) > static_cast<double>(limits_.maxBytes) * 0.9) &&
           DropOneCompactable()) {
    }
  }
  if (over(1.0) && (!r.critical || over(2.0))) {
    gap_ = true;
    metaDirty_ = true;
    ++dropped_;
    if (droppedAny) Rewrite();
    if (why) *why = std::string("spool full (") + std::to_string(recs_.size()) + " msgs, " + std::to_string(bytes_) +
                    " bytes); " + (r.critical ? "critical" : "compactable") + " message " + type + " not spooled";
    return 0;
  }
  r.seq = nextSeq_++;
  metaDirty_ = true;
  bytes_ += add;
  recs_.push_back(r);
  if (droppedAny) {
    Rewrite();
  } else if (!AppendLine(recs_.back())) {
    Rewrite();
  }
  return r.seq;
}

void Spool::AckUpTo(int64_t seq) {
  if (seq <= ackedSeq_) return;
  ackedSeq_ = std::min(seq, lastSeq());
  metaDirty_ = true;
  while (!recs_.empty() && recs_.front().seq <= ackedSeq_) {
    bytes_ -= recs_.front().Bytes();
    recs_.pop_front();
  }
  if ((recs_.empty() && logBytes_ > (64u << 10)) || (logBytes_ > (1u << 20) && logBytes_ > 2 * bytes_ + (1u << 20))) {
    FlushMeta();  // meta first: a crash between the two then only replays already-acked messages
    Rewrite();
  }
}

bool Spool::RotateIfGap() {
  if (!gap_) return false;
  streamId_ = NewUlid(NowMs());
  int64_t s = 0;
  for (auto& r : recs_) r.seq = ++s;
  nextSeq_ = s + 1;
  ackedSeq_ = 0;
  gap_ = false;
  metaDirty_ = true;
  FlushMeta();
  Rewrite();
  return true;
}

void Spool::SetRxSeq(int64_t seq) {
  if (seq == rxSeq_) return;
  rxSeq_ = seq;
  metaDirty_ = true;
}

bool Spool::FlushMeta() {
  if (dir_.empty()) return false;
  json::Value v = json::Value::Object();
  v.Set("stream_id", json::Value::Str(streamId_));
  v.Set("next_seq", json::Value::Int(nextSeq_));
  v.Set("acked_seq", json::Value::Int(ackedSeq_));
  v.Set("rx_seq", json::Value::Int(rxSeq_));
  v.Set("gap", json::Value::Bool(gap_));
  std::string err;
  const bool ok = WriteFileAtomic(dir_ + "/meta.json", json::Dump(v) + "\n", 0600, &err);
  if (ok) metaDirty_ = false;
  return ok;
}

void Spool::MaybeFlush(int64_t nowMs) {
  if (!metaDirty_ || nowMs - lastFlushMs_ < 1000) return;
  lastFlushMs_ = nowMs;
  FlushMeta();
}

}  // namespace fleet
