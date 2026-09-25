#pragma once

// Match features on top of the ready / live flow (docs/PARITY.md §7 P1), engine side. The rules
// and the pure logic are in match_rules.h.
//
// Pauses (chat, roster players, live map only):
//   .tac               tactical timeout: CS2's own timeout_ct_start / timeout_terrorist_start for
//                      the caller's side (count and length: mp_team_timeout_max / _time). Ready Up
//                      shows it as a "tactical" pause and clears it at the next round_freeze_end.
//   .pause .p .tech    technical pause: mp_pause_match (takes effect at freeze time), at most
//                      max_tech_pauses_per_team per team per map; after tech_pause_max_seconds of
//                      pause the match unpauses by itself (countdown in the center HUD).
//   .unpause .up       both teams (both_teams_unpause_required=1) or the pausing team alone.
//                      An admin pause only ends with .forceunpause.
//   .forcepause .fp / .forceunpause .fup   admins (same as `.ru fp` / `.ru fup`).
// .forceready          (allow_force_ready) readies the caller's whole team in warmup once it has
//                      min_players_to_ready connected (0 = the full roster).
// Forfeit              a team with nobody connected for forfeit_after_seconds while a map is live
//                      forfeits the map and the series (modes.h ForfeitCurrentMap). Countdown in
//                      chat and the HUD; cancelled when someone reconnects.
//
// Admin / console, on behalf of a team: `ru tech team1|team2`, `ru tac team1|team2` (same limits).
// Log lines (scripts/livetest): `pause: ...`, `forfeit: ...`.

#include "readyup/match_rules.h"
#include "readyup/webhook.h"

#include <cstdint>
#include <string>

namespace readyup {

// The match's rules resolved against readyup.cfg and the built-in defaults. Any thread.
MatchRules EffectiveRules();

// Engine game events (synchronous, game thread): round_start, round_freeze_end.
void MatchFeaturesOnGameEvent(const char* name);

// Game thread, every frame: tactical timeout end, technical pause auto-unpause, forfeit timer.
void MatchFeaturesTick();

// Pause commands for `team` (steamid64 = the player, 0 = console / admin on behalf of the team).
// Reply goes to chat (players) or the console. Game thread.
void MatchFeaturesTechPause(WebhookTeam team, uint64_t steamid64, const std::string& name);
void MatchFeaturesTacticalTimeout(WebhookTeam team, uint64_t steamid64, const std::string& name);
void MatchFeaturesUnpause(WebhookTeam team, uint64_t steamid64, const std::string& name);
// `.forceready` from a roster player (match) or a player on CT/T (scrim warmup: ctx empty).
void MatchFeaturesForceReady(uint64_t steamid64, const std::string& name);

// Center HUD while a map is live (ready_hud.cpp).
struct LiveHudInfo {
  bool paused = false;
  std::string type;            // "tactical" | "technical" | "admin"
  std::string byTeam;          // pausing team's name (empty for admins)
  bool pendingFreeze = false;  // technical: waiting for freeze time
  int secondsLeft = -1;        // technical auto-unpause countdown (-1 = none)
  bool team1Confirmed = false, team2Confirmed = false;
  bool bothRequired = true;
  std::string team1, team2;  // team names
  bool forfeit = false;
  std::string forfeitTeam;  // the absent team
  int forfeitSecondsLeft = 0;
};
LiveHudInfo MatchFeaturesHud();

}  // namespace readyup
