#pragma once

// Pause / ready / forfeit rules of a match and the pure logic behind them (no engine calls; unit
// tested by ctest `match_rules`). The engine side lives in match_features.h.
//
// Where a value comes from (first one set wins):
//   1. the match config: MAT JSON top-level keys `max_tech_pauses_per_team`, `tech_pause_max_seconds`,
//      `both_teams_unpause_required`, `allow_force_ready`, `min_players_to_ready`,
//      `forfeit_after_seconds`; a fleet `match.assign` maps `rules.pause.technical_per_team`,
//      `rules.pause.technical_seconds`, `rules.pause.unpause`, `rules.ready.allow_force_ready`,
//      `rules.ready.min_per_team` and `rules.forfeit.team_absent_seconds` onto them
//      (fleet_state.cpp AssignToMatConfig),
//   2. readyup.cfg / match.cfg keys of the same names (scrims only have these),
//   3. the built-in defaults (BuiltinDefaultRules).
//
// Tactical timeouts (`.tac`) are CS2's own (timeout_ct_start / timeout_terrorist_start): their
// count and length are mp_team_timeout_max / mp_team_timeout_time (live.cfg, match cvars).

#include <string>

namespace readyup {

struct MatchRules {
  // -1 = not set (take the next source).
  int tech_pauses_per_team = -1;   // technical pauses per team per map; 0 = unlimited
  int tech_pause_max_seconds = -1; // auto-unpause after this long; 0 = no limit
  int both_teams_unpause = -1;     // 1: both teams type .unpause; 0: the pausing team alone
  int allow_force_ready = -1;      // `.forceready`
  int min_players_to_ready = -1;   // connected players a team needs for .forceready; 0 = full roster
  int forfeit_after_seconds = -1;  // whole team disconnected while live -> forfeit; 0 = off
};

MatchRules BuiltinDefaultRules();
// Every -1 in `match` is taken from `base`, then from BuiltinDefaultRules(). The result has no -1.
MatchRules ResolveRules(const MatchRules& match, const MatchRules& base);

// ---- pauses ----------------------------------------------------------------------------------

// A team with `used` technical pauses this map may pause again under `limit` (0 = unlimited).
bool TechPauseAllowed(int used, int limit);
// Technical pauses left (-1 = unlimited).
int TechPausesLeft(int used, int limit);
// Seconds until the automatic unpause (`elapsed` since the pause took effect), -1 = no limit.
int TechPauseSecondsLeft(int elapsedSeconds, int maxSeconds);

// team: 1 / 2 (0 = unknown). pauserTeam: the team that paused (0 = admin / console / unknown).
// Both required: both teams confirmed. Otherwise the pausing team alone is enough (any team when
// the pauser is not a team).
bool UnpauseSatisfied(bool team1Confirmed, bool team2Confirmed, bool bothRequired, int pauserTeam);

// ---- .forceready -----------------------------------------------------------------------------

// connected: roster players of the team on the server; rosterSize: the team's roster; minPlayers:
// rule (0 = the full roster). Empty rosters never qualify.
bool ForceReadyAllowed(int connected, int rosterSize, int minPlayers);
int ForceReadyRequired(int rosterSize, int minPlayers);

// ---- team names ------------------------------------------------------------------------------

// Value for `mp_teamname_N "<value>"`: control bytes, quotes and ';' dropped, trimmed, at most
// 32 bytes (on a UTF-8 boundary).
std::string SanitizeTeamName(const std::string& in);

// ---- forfeit when a team leaves --------------------------------------------------------------

// One team with nobody connected for `limitSeconds` loses by forfeit. Fed once per tick.
class ForfeitTimer {
 public:
  enum class Action { None, Started, Cancelled, Expired };
  struct Result {
    Action action = Action::None;
    int absentTeam = 0;   // 1 / 2 while counting (and with Expired)
    int secondsLeft = 0;  // while counting
  };

  // now: seconds (monotonic). active: the map is live and the rule is on (limitSeconds > 0).
  // Both teams gone (empty server) or both present: no countdown.
  Result Update(double now, bool active, int limitSeconds, bool team1Present, bool team2Present);
  void Reset();

  bool Running() const { return absent_ != 0 && !fired_; }
  int AbsentTeam() const { return absent_; }
  int SecondsLeft(double now) const;

 private:
  int absent_ = 0;
  double deadline_ = 0;
  bool fired_ = false;
};

}  // namespace readyup
