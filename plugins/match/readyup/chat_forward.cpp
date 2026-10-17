#include "readyup/chat_forward.h"

#include "readyup/admin_call_logic.h"
#include "readyup/steamid.h"

namespace readyup::chat_forward {

namespace {

// The <> groups of a player header: name<userid><steamid><team>.
bool HeaderGroups(const std::string& header, std::string* name, std::string* id, std::string* team) {
  const size_t a = header.find('<');
  if (a == std::string::npos) return false;
  const size_t b = header.find('>', a + 1);
  const size_t c = b == std::string::npos ? std::string::npos : header.find('<', b + 1);
  const size_t d = c == std::string::npos ? std::string::npos : header.find('>', c + 1);
  const size_t e = d == std::string::npos ? std::string::npos : header.find('<', d + 1);
  const size_t f = e == std::string::npos ? std::string::npos : header.find('>', e + 1);
  if (f == std::string::npos) return false;
  *name = header.substr(0, a);
  *id = header.substr(c + 1, d - (c + 1));
  *team = header.substr(e + 1, f - (e + 1));
  return true;
}

}  // namespace

std::optional<ChatLine> ParseChatLogLine(const std::string& line) {
  // The header is the first quoted part; it ends right before `" say "` / `" say_team "`.
  const size_t q = line.find('"');
  if (q == std::string::npos) return std::nullopt;
  static const std::string kSay = ">\" say \"", kSayTeam = ">\" say_team \"";
  const size_t s1 = line.find(kSay, q + 1);
  const size_t s2 = line.find(kSayTeam, q + 1);
  size_t at = std::string::npos, msgStart = std::string::npos;
  bool teamOnly = false;
  if (s2 != std::string::npos && (s1 == std::string::npos || s2 < s1)) {
    at = s2;
    msgStart = s2 + kSayTeam.size();
    teamOnly = true;
  } else if (s1 != std::string::npos) {
    at = s1;
    msgStart = s1 + kSay.size();
  } else {
    return std::nullopt;
  }
  // The message runs to the line's last quote (it may hold quotes itself).
  const size_t msgEnd = line.rfind('"');
  if (msgEnd == std::string::npos || msgEnd < msgStart) return std::nullopt;

  std::string name, id, team;
  if (!HeaderGroups(line.substr(q + 1, at + 1 - (q + 1)), &name, &id, &team)) return std::nullopt;
  if (id == "BOT" || id == "Console") return std::nullopt;
  const uint64_t sid = ParseSteamId64Loose(id);
  if (sid == 0) return std::nullopt;

  ChatLine c;
  c.steamid64 = sid;
  c.name = name;
  c.side = team == "CT" ? "ct" : team == "TERRORIST" ? "t" : "spec";
  c.team_only = teamOnly;
  c.message = line.substr(msgStart, msgEnd - msgStart);
  return c;
}

bool IsCommand(const std::string& message) {
  const size_t i = message.find_first_not_of(' ');
  if (i == std::string::npos || i + 1 >= message.size()) return false;
  return message[i] == '.' || message[i] == '!';
}

bool RateLimiter::Allow(uint64_t steamid64, double now) {
  auto& q = seen_[steamid64];
  while (!q.empty() && now - q.front() >= window_) q.pop_front();
  if (static_cast<int>(q.size()) >= max_) return false;
  q.push_back(now);
  // Players who left: drop their empty windows now and then.
  if (seen_.size() > 256) {
    for (auto it = seen_.begin(); it != seen_.end();) {
      if (it->second.empty() || now - it->second.back() >= window_) it = seen_.erase(it);
      else ++it;
    }
  }
  return true;
}

status::Json EventData(const ChatLine& c, const std::string& team, long long atUnixMs) {
  status::Json player = status::Json::Object();
  player["steamid64"] = std::to_string(c.steamid64);
  player["name"] = CleanAdminCallMessage(c.name, 64);
  player["team"] = team == "team1" || team == "team2" ? status::Json(team) : status::Json();
  player["side"] = c.side;
  status::Json d = status::Json::Object();
  d["player"] = std::move(player);
  d["message"] = CleanAdminCallMessage(c.message, kMessageMaxChars);
  d["team_only"] = c.team_only;
  d["at"] = atUnixMs;
  return d;
}

}  // namespace readyup::chat_forward
