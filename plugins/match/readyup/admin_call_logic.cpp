#include "readyup/admin_call_logic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace readyup {

bool AdminCallCooldown::TryCall(uint64_t steamid64, double now, int cooldownSeconds, int* secondsLeft) {
  if (secondsLeft) *secondsLeft = 0;
  if (cooldownSeconds <= 0) {
    last_[steamid64] = now;
    return true;
  }
  const auto it = last_.find(steamid64);
  if (it != last_.end()) {
    const double left = it->second + cooldownSeconds - now;
    if (left > 0) {
      if (secondsLeft) *secondsLeft = std::max(1, static_cast<int>(std::ceil(left)));
      return false;
    }
  }
  last_[steamid64] = now;
  // Keep the map small: forget players whose cooldown ran out long ago.
  if (last_.size() > 256) {
    for (auto i = last_.begin(); i != last_.end();) {
      if (now - i->second > cooldownSeconds) i = last_.erase(i);
      else ++i;
    }
  }
  return true;
}

std::string CleanAdminCallMessage(const std::string& raw, size_t maxChars) {
  std::string s;
  s.reserve(raw.size());
  for (char ch : raw) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '\t' || c == '\n' || c == '\r' || c == ' ') {
      if (!s.empty() && s.back() != ' ') s += ' ';
    } else if (c >= 0x20 && c != 0x7F) {
      s += ch;
    }
  }
  while (!s.empty() && s.back() == ' ') s.pop_back();
  // Cut to maxChars code points (a lead byte starts a new character).
  size_t chars = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) continue;
    if (chars == maxChars) {
      s.resize(i);
      while (!s.empty() && s.back() == ' ') s.pop_back();
      break;
    }
    ++chars;
  }
  return s;
}

std::string IsoUtcFromUnixMs(long long unixMs) {
  if (unixMs < 0) unixMs = 0;
  const std::time_t secs = static_cast<std::time_t>(unixMs / 1000);
  std::tm tm{};
  gmtime_r(&secs, &tm);
  char buf[80];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(unixMs % 1000));
  return buf;
}

std::string CallIdFromBytes(const unsigned char bytes[16]) {
  unsigned char b[16];
  for (int i = 0; i < 16; ++i) b[i] = bytes[i];
  b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);  // version 4
  b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);  // RFC 4122 variant
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
    out += kHex[b[i] >> 4];
    out += kHex[b[i] & 0x0F];
  }
  return out;
}

status::Json AdminCallData(const AdminCallEvent& e) {
  status::Json player = status::Json::Object();
  player["steamid64"] = std::to_string(e.player.steamid64);
  player["name"] = e.player.name;
  player["team"] = e.player.team.empty() ? status::Json() : status::Json(e.player.team);
  player["side"] = e.player.side.empty() ? status::Json() : status::Json(e.player.side);
  status::Json d = status::Json::Object();
  d["call_id"] = e.call_id;
  d["player"] = std::move(player);
  d["message"] = e.message;
  d["called_at"] = e.called_at;
  return d;
}

std::string AdminCalledWebhookJson(const AdminCallEvent& e) {
  status::Json j = status::Json::Object();
  j["event"] = "admin_called";
  j["matchid"] = e.hasMatch ? status::Json(static_cast<unsigned long long>(e.matchid)) : status::Json();
  j["map_number"] = e.map_number;
  const status::Json d = AdminCallData(e);
  for (const auto& kv : d.Members()) j[kv.first] = kv.second;
  return j.Dump();
}

std::string AdminCallTeamLabel(const std::string& teamName, const std::string& side, bool spectator) {
  if (spectator) return "spectator";
  const std::string s = side == "ct" ? "CT" : side == "t" ? "T" : "";
  if (teamName.empty()) return s;
  return s.empty() ? teamName : teamName + ", " + s;
}

}  // namespace readyup
