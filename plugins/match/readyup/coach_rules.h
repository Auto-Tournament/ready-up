#pragma once

// Coaches (coach.h), the engine-free half: who may coach which team, the CS2 side a coach watches
// and the admin command's player lookup. ctest `match_coach_rules`.
//
// Teams are the match teams: 0 = none, 1 = team1, 2 = team2 (WebhookTeam values). Sides are CS2
// team numbers: 1 = spectator, 2 = T, 3 = CT.

#include <cstdint>
#include <string>
#include <vector>

namespace readyup {

// Default coaches per team (get5 / MAT `coaches_per_team`) for coaches who are not listed in the
// match config: scrim coaches and admin-assigned ones. Listed coaches are never limited.
constexpr int kDefaultCoachesPerTeam = 2;

// `.coach <arg>` / `ru match coach <player> <arg>`.
enum class CoachArg { None, Ct, T, Team1, Team2, Invalid };
// "ct", "t", "team1", "team2" (any case); "" = None; anything else = Invalid.
CoachArg ParseCoachArg(const std::string& arg);

struct CoachRequest {
  bool coaching_possible = false;  // a match or scrim is loaded, or scrim warmup (not idle / practice)
  bool scrim = false;              // scrim (open): anyone who is not a player may coach
  bool admitted = true;            // the ruleset admits coaches (ruleset.h CoachesAdmitted)
  bool by_admin = false;           // `ru match coach` (an admin or the console)
  int roster_team = 0;             // the player's roster team, 0 = not a rostered player
  bool listed = false;             // in the match config's coaches
  int listed_team = 0;             // 0 = listed without a team (MAT top-level "coaches": [...])
  int current_team = 0;            // the team the player coaches now, 0 = none
  CoachArg arg = CoachArg::None;
  bool team1_is_ct = true;         // team1's side right now (ct / t name a side)
  int coaches_team1 = 0;           // active coaches of each team, the player excluded
  int coaches_team2 = 0;
  int per_team_limit = kDefaultCoachesPerTeam;  // <= 0: no limit
};

struct CoachDecision {
  bool ok = false;
  int team = 0;         // ok: the team to coach
  bool already = false; // ok: the player already coaches it
  std::string reason;   // !ok: why, for the player ("you are on team1's roster: ...")
};

// The whole policy:
//   - a match / scrim (or scrim warmup) is needed, and the ruleset must admit coaches;
//   - rostered players never coach (they would leave a player slot empty);
//   - a coach listed with a team coaches that team (`.coach` alone is enough); an admin may move them;
//   - outside scrims only listed coaches coach, unless an admin assigns one;
//   - unlisted coaches count against per_team_limit.
// `names` label the teams in the reasons ("team1" / "team2" when empty).
CoachDecision DecideCoach(const CoachRequest& r, const std::string& team1Name = "", const std::string& team2Name = "");

// Where each team's players are right now (connected rostered players on T / CT).
struct SideTally {
  int team1_ct = 0, team1_t = 0;
  int team2_ct = 0, team2_t = 0;
};

// team1's side from the players: true = CT. Undecided (nobody on a side, or a tie while the teams
// swap) = the fallback.
bool Team1IsCtFromTally(const SideTally& t, bool fallbackTeam1IsCt);

// The CS2 side (2 T / 3 CT) the coach of `team` (1 / 2) should watch, i.e. what the controller's
// m_iCoachingTeam should hold while the coach is a spectator:
//   - the side where most of that team's players are, else the opposite of the other team's;
//   - undecided: keep `current` (the engine swaps coaches with their team at halftime and on
//     mp_swapteams, so a value it set is right), else the fallback (team1_is_ct from the map
//     sides and the halves played).
// 0 for team 0.
int CoachingSide(int team, const SideTally& t, int current, bool fallbackTeam1IsCt);

// A connected human, for the admin command's player argument.
struct CoachCandidate {
  uint64_t steamid64 = 0;
  int userid = -1;  // the log `<N>` (what `status` / kickid show)
  std::string name;
};

// `ru match coach <player>`: a SteamID (64, [U:1:x], STEAM_x:y:z) of a connected player, `#<userid>`,
// or a name: an exact (case-insensitive) match first, else a unique substring. 0 and *err otherwise.
uint64_t ResolveCoachPlayer(const std::string& arg, const std::vector<CoachCandidate>& players, std::string* err);

// "team1" / "team2" or the team's name.
std::string CoachTeamLabel(int team, const std::string& team1Name, const std::string& team2Name);

}  // namespace readyup
