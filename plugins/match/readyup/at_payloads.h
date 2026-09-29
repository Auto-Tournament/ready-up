#pragma once

// Auto Tournament CS2 ("AT") webhook payloads (docs/PARITY.md §5, §6, §8) built from Ready Up's own
// models: the stats model (match_stats.h) and DemoEvent (demo_recorder.h).
//
// Engine-free and pure (tests/match_flow_test.cpp). webhook.cpp puts the result on the event queue;
// the fleet link carries the models themselves (event.round_end / event.map_result / event.demo).
//
// The shapes are the AT plugin's (Events.cs, MatchData.cs): the same field names, the winner as an
// object, team1 / team2 objects with the players and their stats. Ready Up adds a few flat fields
// the AT plugin did not send (map_name, team1_score, team2_score); the platform normalizer reads
// them as fallbacks. Map numbers are Ready Up's (1-based), as in every other Ready Up webhook event.

#include "readyup/demo_recorder.h"
#include "readyup/match_stats.h"

#include <string>
#include <vector>

namespace readyup::at {

// AT `PlayerStats`, from the model:
//   first_kills_t / _ct, first_deaths_t / _ct  = entry_kills_* / entry_deaths_*
//   1k..5k = multi_kills, 1v1..1v5 = clutches_won
//   kast = KAST in percent of rounds_played (0-100, rounded): the platform stores and averages it
//          as a percentage (player_match_stats.kast). The AT plugin always sent 0.
std::string PlayerStatsJson(const stats::PlayerStats& s);
int KastPercent(const stats::PlayerStats& s);

// {"side":"3","team":"team1"}: side = the winner's CS team number as a string (2 = T, 3 = CT,
// "0" = nobody), team = "team1" | "team2" | "none".
std::string WinnerJson(int side, int teamSlot);

struct TeamInfo {
  std::string id;    // match config team1.id / team2.id ("" when the config has none)
  std::string name;
  int series_score = 0;  // maps won
};

// AT `StatsTeam`: id, name, series_score, score, score_ct, score_t, players[{steamid, name, stats}].
// players: the map's players on team slot `slot` (1 / 2); bots are left out.
std::string StatsTeamJson(const TeamInfo& team, int slot, int score, const stats::TeamLine& line,
                          const std::vector<stats::PlayerLine>& players);

// round_end, after the stats model closed the round (stats.rounds.back() is this round).
struct RoundEnd {
  long long matchid = 0;
  int map_number = 1;
  int round_number = 0;
  int round_time_ms = 0;  // since the freeze time ended (Get5 / AT: `round_time`)
  int reason = 0;         // engine round_end reason
  int winner_side = 0;    // 2 / 3 (else nobody)
  TeamInfo team1, team2;  // series score before this map ends
  stats::MapStats stats;
};
std::string RoundEndJson(const RoundEnd& r);

// map_result with the final per-player stats of the map.
struct MapResult {
  long long matchid = 0;
  int map_number = 1;
  std::string map_name;
  int team1_score = 0;
  int team2_score = 0;
  std::string winner;     // "team1" | "team2"; anything else = no winner (draw)
  TeamInfo team1, team2;  // series score after this map
  stats::MapStats stats;
};
std::string MapResultJson(const MapResult& m);

// `ru_match_stats` (the old plugin's get_match_stats; one console line "match_stats {...}"): the
// current map's stats so far, in the shape the platform already reads from round_end / map_result
// (team1 / team2 = StatsTeamJson, per-player AT PlayerStats), so any consumer of those events can
// read it too:
//   {"event":"match_stats","matchid","map_number","map_name","round_number","live",
//    "team1":{...},"team2":{...},"team1_score","team2_score"}
// Scores are the stats model's (round winners through the sides). matchid 0 = no match loaded
// (a scrim's stats; team ids / names empty).
struct MatchStatsLine {
  long long matchid = 0;
  int map_number = 1;
  std::string map_name;
  TeamInfo team1, team2;  // series score so far
  stats::MapStats stats;
};
std::string MatchStatsJson(const MatchStatsLine& m);

// The AT demo events for one DemoEvent (AT Events.cs, `filename`, `size_mb`, `status`, `reason`,
// `success`):
//   RecordingStarted -> demo_recording_start     RecordingStopped -> demo_recording_stop
//   UploadStarted    -> demo_upload_start
//   UploadSucceeded  -> demo_upload_success (status = HTTP status), demo_upload_ended {success: true}
//   UploadFailed     -> demo_upload_fail (status = HTTP status | "file_not_found" | "no_response",
//                       reason; size_mb null when the file was not found), demo_upload_ended {success: false}
// The platform turns a server over once every map's upload ended (utils/serverTurnover.ts).
std::vector<std::string> DemoEventJsons(const demo::DemoEvent& e);

}  // namespace readyup::at
