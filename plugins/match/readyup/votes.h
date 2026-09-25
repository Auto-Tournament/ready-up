#pragma once

// Player votes on a live map (docs/PARITY.md §7), engine side. Pure logic: vote_logic.h.
//
//   .gg    (gg_enabled) surrender vote of the caller's team: gg_threshold of the team's connected
//          players (default 80%) within 60 s, while the team trails by at least gg_min_score_diff
//          rounds (default 8). It passes -> the team forfeits the map and the series through the
//          forfeit flow (modes.h ForfeitCurrentMap, reason "gg": match_forfeit, map_result,
//          series_end). With gg off, `.gg` only emits player_gg as before.
//   .stop  (stop_command_available) one player of each team within stop_vote_seconds (30):
//          restore the start of the current round from CS2's round backup
//          (fleet_bridge::RestoreRoundFromLocalBackup, autopaused). stop_command_no_damage: not
//          once a player damaged an opponent this round.
//
// Under the valve ruleset both are off (ruleset.h PlayerExtrasAllowed): `.gg` only emits
// player_gg, `.stop` is ignored.
//
// Log lines: `vote: ...`.

#include "readyup/plugin_api.h"
#include "readyup/webhook.h"

#include <cstdint>
#include <string>

namespace readyup {

// readyup_plugin_load: round_start (a new round clears a pending .stop).
void VotesInstall(const ru_api* api);
// on_tick: vote timeouts.
void VotesTick(double now);

// voter: SteamID64 (or a dev bot id); team: the voter's match team. Replies in chat. Game thread.
// Return false if the vote is not enabled (the caller keeps its old behaviour).
bool VotesGg(uint64_t voter, WebhookTeam team, const std::string& name);
void VotesStop(uint64_t voter, WebhookTeam team, const std::string& name);

// The match team playing CS side `csTeam` (2 T / 3 CT) on the current map, Unknown if none.
WebhookTeam VotesTeamForSide(int csTeam);

}  // namespace readyup
