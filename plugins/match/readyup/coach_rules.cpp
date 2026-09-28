#include "readyup/coach_rules.h"

#include "readyup/steamid.h"

#include <cctype>
#include <cstdlib>

namespace readyup {

namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

CoachArg ParseCoachArg(const std::string& arg) {
  const std::string a = Lower(arg);
  if (a.empty()) return CoachArg::None;
  if (a == "ct") return CoachArg::Ct;
  if (a == "t") return CoachArg::T;
  if (a == "team1") return CoachArg::Team1;
  if (a == "team2") return CoachArg::Team2;
  return CoachArg::Invalid;
}

std::string CoachTeamLabel(int team, const std::string& team1Name, const std::string& team2Name) {
  if (team == 1) return team1Name.empty() ? std::string("team1") : team1Name;
  if (team == 2) return team2Name.empty() ? std::string("team2") : team2Name;
  return "no team";
}

CoachDecision DecideCoach(const CoachRequest& r, const std::string& team1Name, const std::string& team2Name) {
  CoachDecision d;
  auto label = [&](int team) { return CoachTeamLabel(team, team1Name, team2Name); };
  auto refuse = [&](std::string why) {
    d.ok = false;
    d.team = 0;
    d.reason = std::move(why);
    return d;
  };

  if (!r.coaching_possible) return refuse("coaching needs a match or a scrim (not in practice or idle mode)");
  if (!r.admitted) return refuse("coaches are not admitted in this match (online, lan false, coaches_online false)");
  if (r.roster_team == 1 || r.roster_team == 2) {
    return refuse((r.by_admin ? std::string("they are") : std::string("you are")) + " on " + label(r.roster_team) +
                  "'s roster: players cannot coach");
  }

  int target = 0;
  switch (r.arg) {
    case CoachArg::Ct: target = r.team1_is_ct ? 1 : 2; break;
    case CoachArg::T: target = r.team1_is_ct ? 2 : 1; break;
    case CoachArg::Team1: target = 1; break;
    case CoachArg::Team2: target = 2; break;
    case CoachArg::None:
      if (r.listed && (r.listed_team == 1 || r.listed_team == 2)) target = r.listed_team;
      break;
    case CoachArg::Invalid: break;
  }
  if (target == 0) return refuse(r.by_admin ? "usage: ru match coach <player> team1|team2|ct|t" : "usage: .coach ct|t");

  const bool listedForTarget = r.listed && (r.listed_team == 0 || r.listed_team == target);
  if (r.listed && r.listed_team != 0 && r.listed_team != target && !r.by_admin) {
    return refuse("you are listed as " + label(r.listed_team) + "'s coach");
  }
  if (!r.listed && !r.scrim && !r.by_admin) {
    return refuse("only the coaches in the match config can coach (an admin can add you: .admin)");
  }

  if (r.current_team == target) {
    d.ok = true;
    d.team = target;
    d.already = true;
    return d;
  }

  if (!listedForTarget && r.per_team_limit > 0) {
    const int n = target == 1 ? r.coaches_team1 : r.coaches_team2;
    if (n >= r.per_team_limit) {
      return refuse(label(target) + " already has " + std::to_string(n) + " coach" + (n == 1 ? "" : "es"));
    }
  }
  d.ok = true;
  d.team = target;
  return d;
}

bool Team1IsCtFromTally(const SideTally& t, bool fallbackTeam1IsCt) {
  if (t.team1_ct != t.team1_t) return t.team1_ct > t.team1_t;
  if (t.team2_ct != t.team2_t) return t.team2_t > t.team2_ct;
  return fallbackTeam1IsCt;
}

int CoachingSide(int team, const SideTally& t, int current, bool fallbackTeam1IsCt) {
  if (team != 1 && team != 2) return 0;
  const bool decided = t.team1_ct != t.team1_t || t.team2_ct != t.team2_t;
  if (!decided && (current == 2 || current == 3)) return current;
  const bool team1Ct = Team1IsCtFromTally(t, fallbackTeam1IsCt);
  const bool ct = team == 1 ? team1Ct : !team1Ct;
  return ct ? 3 : 2;
}

uint64_t ResolveCoachPlayer(const std::string& arg, const std::vector<CoachCandidate>& players, std::string* err) {
  auto fail = [&](const std::string& why) -> uint64_t {
    if (err) *err = why;
    return 0;
  };
  if (arg.empty()) return fail("no player given");
  if (arg[0] == '#' && arg.size() > 1) {
    char* end = nullptr;
    const long id = std::strtol(arg.c_str() + 1, &end, 10);
    if (end && *end == '\0') {
      for (const auto& p : players) {
        if (p.userid == id && p.steamid64) return p.steamid64;
      }
      return fail("no connected player with userid " + arg);
    }
  }
  // A SteamID (a short number such as "123" is a name, not an account).
  if (const uint64_t sid = ParseSteamId64Loose(arg); sid >= 76561197960265728ULL) {
    for (const auto& p : players) {
      if (p.steamid64 == sid) return sid;
    }
    return fail("SteamID " + std::to_string(sid) + " is not connected");
  }
  const std::string want = Lower(arg);
  for (const auto& p : players) {
    if (p.steamid64 && Lower(p.name) == want) return p.steamid64;
  }
  uint64_t found = 0;
  int n = 0;
  for (const auto& p : players) {
    if (!p.steamid64 || Lower(p.name).find(want) == std::string::npos) continue;
    found = p.steamid64;
    ++n;
  }
  if (n == 1) return found;
  if (n == 0) return fail("no connected player matches \"" + arg.substr(0, 32) + "\"");
  return fail(std::to_string(n) + " players match \"" + arg.substr(0, 32) + "\"; use the SteamID or #userid");
}

}  // namespace readyup
