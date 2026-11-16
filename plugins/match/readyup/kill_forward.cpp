#include "readyup/kill_forward.h"

#include "readyup/admin_call_logic.h"
#include "readyup/steamid.h"

#include <cmath>
#include <cstdio>

namespace readyup::kill_forward {

namespace {

std::string Side(const std::string& team) {
  if (team == "CT") return "ct";
  if (team == "TERRORIST") return "t";
  return "spec";
}

// `Name<uid><id><TEAM>` (the text between the token's quotes): the last three groups, the name before.
bool Block(const std::string& s, Fighter* f) {
  if (s.empty() || s.back() != '>') return false;
  size_t end = s.size() - 1;
  std::string g[3];
  for (int i = 2; i >= 0; --i) {
    const size_t lt = s.rfind('<', end);
    if (lt == std::string::npos) return false;
    g[i] = s.substr(lt + 1, end - lt - 1);
    if (i > 0) {
      if (lt == 0 || s[lt - 1] != '>') return false;
      end = lt - 1;
    } else {
      f->name = s.substr(0, lt);
    }
  }
  if (f->name.empty() || g[0].empty() || g[0].find_first_not_of("0123456789") != std::string::npos) return false;
  f->steamid64 = g[1] == "BOT" ? 0 : ParseSteamId64Loose(g[1]);
  if (g[1] != "BOT" && f->steamid64 == 0) return false;
  f->side = Side(g[2]);
  return true;
}

bool Position(const std::string& s, Fighter* f) {
  return std::sscanf(s.c_str(), "[%lf %lf %lf]", &f->x, &f->y, &f->z) == 3;
}

}  // namespace

std::optional<Kill> ParseKillLogLine(const std::string& line) {
  static const std::string kKilled = " killed \"";
  const size_t k = line.find(kKilled);
  if (k == std::string::npos || line.find(kKilled, k + 1) != std::string::npos) return std::nullopt;
  // Attacker: `"A<..><..><..>" [x y z]` ends at the kill word.
  const std::string left = line.substr(0, k);
  const size_t lq = left.find('"');
  const size_t rq = left.rfind("\" [");
  if (lq == std::string::npos || rq == std::string::npos || rq <= lq) return std::nullopt;
  Kill kill;
  if (!Block(left.substr(lq + 1, rq - lq - 1), &kill.attacker) || !Position(left.substr(rq + 2), &kill.attacker)) {
    return std::nullopt;
  }
  // Victim: `B<..><..><..>" [x y z] with "weapon"` (and `(headshot ...)`).
  const std::string right = line.substr(k + kKilled.size());
  const size_t vq = right.find("\" [");
  const size_t with = right.find("] with \"");
  if (vq == std::string::npos || with == std::string::npos || with < vq) return std::nullopt;
  if (!Block(right.substr(0, vq), &kill.victim) || !Position(right.substr(vq + 2, with - vq - 1), &kill.victim)) {
    return std::nullopt;
  }
  const size_t ws = with + 8;
  const size_t we = right.find('"', ws);
  if (we == std::string::npos) return std::nullopt;
  kill.weapon = right.substr(ws, we - ws);
  kill.headshot = right.find("headshot", we) != std::string::npos;
  return kill;
}

status::Json EventData(const Kill& k, long long atUnixMs, double gameTime) {
  const auto fighter = [](const Fighter& f) {
    status::Json j = status::Json::Object();
    j["name"] = CleanAdminCallMessage(f.name, 64);
    j["steamid64"] = f.steamid64 ? status::Json(std::to_string(f.steamid64)) : status::Json();
    j["side"] = f.side;
    status::Json pos = status::Json::Array();
    pos.Push(f.x);
    pos.Push(f.y);
    pos.Push(f.z);
    j["pos"] = std::move(pos);
    return j;
  };
  status::Json d = status::Json::Object();
  d["attacker"] = fighter(k.attacker);
  d["victim"] = fighter(k.victim);
  d["weapon"] = k.weapon;
  d["headshot"] = k.headshot;
  d["at"] = atUnixMs;
  if (std::isfinite(gameTime)) d["game_time"] = gameTime;
  return d;
}

}  // namespace readyup::kill_forward
