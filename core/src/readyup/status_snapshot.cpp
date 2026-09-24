#include "readyup/status_snapshot.h"

#include <cmath>
#include <cstdio>

namespace readyup::status {

// ---------------------------------------------------------------------------- Json

Json& Json::operator[](const std::string& key) {
  if (t_ != Type::Object) {
    *this = Object();
  }
  for (auto& kv : obj_) {
    if (kv.first == key) return kv.second;
  }
  obj_.emplace_back(key, Json());
  return obj_.back().second;
}

const Json* Json::Find(const std::string& key) const {
  if (t_ != Type::Object) return nullptr;
  for (const auto& kv : obj_) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

void Json::Push(Json v) {
  if (t_ != Type::Array) *this = Array();
  arr_.push_back(std::move(v));
}

void JsonEscapeTo(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20 || c == 0x7f) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

void Json::DumpTo(std::string& out) const {
  switch (t_) {
    case Type::Null: out += "null"; return;
    case Type::Bool: out += i_ ? "true" : "false"; return;
    case Type::Int: out += std::to_string(i_); return;
    case Type::Double: {
      if (!std::isfinite(d_)) {
        out += "null";
        return;
      }
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.10g", d_);
      out += buf;
      return;
    }
    case Type::String: JsonEscapeTo(out, s_); return;
    case Type::Object: {
      out += '{';
      bool first = true;
      for (const auto& kv : obj_) {
        if (!first) out += ',';
        first = false;
        JsonEscapeTo(out, kv.first);
        out += ':';
        kv.second.DumpTo(out);
      }
      out += '}';
      return;
    }
    case Type::Array: {
      out += '[';
      for (size_t i = 0; i < arr_.size(); ++i) {
        if (i) out += ',';
        arr_[i].DumpTo(out);
      }
      out += ']';
      return;
    }
  }
}

std::string Json::Dump() const {
  std::string s;
  DumpTo(s);
  return s;
}

bool Json::operator==(const Json& o) const {
  if (t_ != o.t_) return false;
  switch (t_) {
    case Type::Null: return true;
    case Type::Bool:
    case Type::Int: return i_ == o.i_;
    case Type::Double: return d_ == o.d_;
    case Type::String: return s_ == o.s_;
    case Type::Array: return arr_ == o.arr_;
    case Type::Object: {
      if (obj_.size() != o.obj_.size()) return false;
      for (const auto& kv : obj_) {
        const Json* other = o.Find(kv.first);
        if (!other || !(kv.second == *other)) return false;
      }
      return true;
    }
  }
  return false;
}

bool MergeDiff(const Json& from, const Json& to, Json* patch) {
  if (from == to) return false;
  if (!from.IsObject() || !to.IsObject()) {
    // Replace whole. (A null target cannot be expressed inside an object patch other than
    // by deleting the key; at the top level null means "no state", which is what we want.)
    if (patch) *patch = to;
    return true;
  }
  Json p = Json::Object();
  for (const auto& kv : from.Members()) {
    if (!to.Find(kv.first)) p[kv.first] = Json();
  }
  for (const auto& kv : to.Members()) {
    const Json* old = from.Find(kv.first);
    if (!old) {
      p[kv.first] = kv.second;
      continue;
    }
    Json sub;
    if (kv.second.IsNull()) {
      // Merge patch cannot set a member to null; drop it (consumers treat absent == null).
      if (!old->IsNull()) p[kv.first] = Json();
      continue;
    }
    if (MergeDiff(*old, kv.second, &sub)) p[kv.first] = std::move(sub);
  }
  if (p.Members().empty()) return false;  // differed only in null members vs absent keys
  if (patch) *patch = std::move(p);
  return true;
}

Json MergePatchApply(Json target, const Json& patch) {
  if (!patch.IsObject()) return patch;
  if (!target.IsObject()) target = Json::Object();
  Json out = Json::Object();
  // Keep target members not deleted by the patch, in order.
  for (const auto& kv : target.Members()) {
    const Json* p = patch.Find(kv.first);
    if (!p) {
      out[kv.first] = kv.second;
    } else if (!p->IsNull()) {
      out[kv.first] = MergePatchApply(kv.second, *p);
    }
  }
  for (const auto& kv : patch.Members()) {
    if (target.Find(kv.first) || kv.second.IsNull()) continue;
    out[kv.first] = MergePatchApply(Json(), kv.second);
  }
  return out;
}

std::string SseFrame(const std::string& event, const std::string& id, const std::string& data) {
  std::string f;
  f.reserve(data.size() + event.size() + id.size() + 32);
  if (!event.empty()) f += "event: " + event + "\n";
  if (!id.empty()) f += "id: " + id + "\n";
  size_t start = 0;
  for (;;) {
    size_t nl = data.find('\n', start);
    std::string line = data.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    f += "data: ";
    f += line;
    f += '\n';
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  f += '\n';
  return f;
}

// ---------------------------------------------------------------------------- Hub

Hub::Hub(size_t patchRing) : cap_(patchRing ? patchRing : 1) {}

void Hub::Submit(std::shared_ptr<const StatusInputs> in) {
  std::lock_guard<std::mutex> lk(mu_);
  pending_ = std::move(in);
  ++submissions_;
}

uint64_t Hub::Submissions() const {
  std::lock_guard<std::mutex> lk(mu_);
  return submissions_;
}

void Hub::Append(uint64_t rev, std::string frame) {
  StreamEvent ev;
  ev.seq = ++lastSeq_;
  ev.rev = rev;
  ev.frame = std::move(frame);
  events_.push_back(std::move(ev));
  // Keep the last `cap_` patch revs (status events ride along; bound the total at 2x).
  size_t patches = 0;
  for (const auto& e : events_) patches += e.rev ? 1 : 0;
  while (!events_.empty() && (patches > cap_ || events_.size() > cap_ * 2)) {
    if (events_.front().rev) --patches;
    oldestDroppedSeq_ = events_.front().seq;
    events_.pop_front();
  }
}

bool Hub::Process() {
  std::shared_ptr<const StatusInputs> in;
  {
    std::lock_guard<std::mutex> lk(mu_);
    in = std::move(pending_);
    pending_.reset();
  }
  if (!in) return false;

  const std::string versionsJ = in->versions.Dump();
  const std::string selftestJ = in->selftest.Dump();
  const std::string platformJ = in->platform.Dump();
  const std::string summaryJ = in->summary.Dump();
  const std::string stateJ = in->state.Dump();

  const bool first = !cur_;
  bool appended = false;
  if (first) {
    rev_ = 1;
  } else {
    if (stateJ != stateJ_) {
      Json patch;
      if (MergeDiff(cur_->state, in->state, &patch)) {
        ++rev_;
        std::string data = "{\"rev\":" + std::to_string(rev_) + ",\"patch\":";
        patch.DumpTo(data);
        data += '}';
        Append(rev_, SseFrame("patch", std::to_string(rev_), data));
        appended = true;
      }
    }
    std::string data;
    auto add = [&data](const char* key, const std::string& json) {
      data += data.empty() ? "{" : ",";
      data += '"';
      data += key;
      data += "\":";
      data += json;
    };
    if (platformJ != platformJ_) add("platform", platformJ);
    if (in->update_safe != cur_->update_safe) add("update_safe", in->update_safe ? "true" : "false");
    if (summaryJ != summaryJ_) add("summary", summaryJ);
    if (versionsJ != versionsJ_) add("versions", versionsJ);
    if (selftestJ != selftestJ_) add("selftest", selftestJ);
    if (in->healthy != cur_->healthy) add("healthy", in->healthy ? "true" : "false");
    if (!data.empty()) {
      data += '}';
      Append(0, SseFrame("status", "", data));
      appended = true;
    }
  }

  cur_ = std::move(in);
  versionsJ_ = versionsJ;
  selftestJ_ = selftestJ;
  platformJ_ = platformJ;
  summaryJ_ = summaryJ;
  stateJ_ = stateJ;
  return appended || first;
}

std::string Hub::StatusBody(long long uptimeS, long long nowMs) const {
  std::string b;
  if (!cur_) return "{\"error\":\"starting\"}";
  b.reserve(stateJ_.size() + summaryJ_.size() + versionsJ_.size() + selftestJ_.size() + platformJ_.size() + 256);
  b += '{';
  if (!cur_->server_id.empty()) {
    b += "\"server_id\":";
    JsonEscapeTo(b, cur_->server_id);
    b += ',';
  }
  b += "\"hostname\":";
  JsonEscapeTo(b, cur_->hostname);
  b += ",\"game_port\":" + std::to_string(cur_->game_port);
  b += ",\"uptime_s\":" + std::to_string(uptimeS);
  b += ",\"generated_at\":" + std::to_string(nowMs);
  b += ",\"rev\":" + std::to_string(rev_);
  b += ",\"versions\":" + versionsJ_;
  b += ",\"selftest\":" + selftestJ_;
  b += ",\"platform\":" + platformJ_;
  b += ",\"update_safe\":";
  b += cur_->update_safe ? "true" : "false";
  b += ",\"summary\":" + summaryJ_;
  b += ",\"state\":" + stateJ_;
  b += '}';
  return b;
}

std::string Hub::SnapshotFrame() const {
  if (!cur_) return SseFrame("snapshot", "", "{}");
  std::string d = "{\"rev\":" + std::to_string(rev_);
  d += ",\"summary\":" + summaryJ_;
  d += ",\"platform\":" + platformJ_;
  d += ",\"update_safe\":";
  d += cur_->update_safe ? "true" : "false";
  d += ",\"versions\":" + versionsJ_;
  d += ",\"selftest\":" + selftestJ_;
  d += ",\"state\":" + stateJ_;
  d += '}';
  return SseFrame("snapshot", std::to_string(rev_), d);
}

bool Hub::FramesAfter(uint64_t afterSeq, std::string* out, uint64_t* lastSeq) const {
  if (lastSeq) *lastSeq = lastSeq_;
  if (afterSeq >= lastSeq_) return true;
  if (afterSeq < oldestDroppedSeq_) return false;
  for (const auto& e : events_) {
    if (e.seq > afterSeq && out) *out += e.frame;
  }
  return true;
}

bool Hub::SeqForRev(uint64_t rev, uint64_t* seq) const {
  // Only a rev whose patch event is still in the ring can be resumed exactly (the status
  // events after it are replayed too); anything else gets a fresh snapshot.
  for (const auto& e : events_) {
    if (e.rev == rev) {
      if (seq) *seq = e.seq;
      return true;
    }
  }
  return false;
}

}  // namespace readyup::status
