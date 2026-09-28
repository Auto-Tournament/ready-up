#pragma once

// Coaches: CS2's own coach slot, driven by Ready Up (docs/PARITY.md §7, "Coach").
//
// CS2 still has the CS:GO coaching code: a spectator whose controller has m_iCoachingTeam = 2|3
// counts as that team's member for team chat and voice, can only spectate that team under
// mp_forcecamera 1, is swapped with the team at halftime / mp_swapteams and is kept in the round
// backups ("Coaches/<n>"), all while sv_coaching_enabled is 1. The `coach` console command that
// set the field is gone from CS2, so Ready Up sets it (schema field, no signature):
//
//   - `.coach ct|t` (or `.coach` alone for a coach listed with a team) / `.uncoach` in chat;
//     `ru match coach <player> team1|team2|ct|t` / `ru match uncoach <player>` for admins.
//   - Coaches listed per team in the match config (MAT team1/team2 `coaches`, fleet role
//     `coach`) coach their team as soon as they are spectators; nothing to type.
//   - Coaches stay spectators: never a player slot, never in the ready gate, the auto_5v5 count,
//     the forfeit check or the whitelist's roster. A coach who joins CT/T is told to go back to
//     Spectators and coaches again once there.
//   - The side follows the team (knife `.switch`, halftime, overtime): the side most of the
//     team's players are on, the engine's own swap in between.
//   - Reconnects and map changes: the coach is re-applied when the player is a spectator again.
//   - sv_coaching_enabled is 1 while anyone coaches, 0 again after.
// Policy (coach_rules.h): rostered players never coach; outside scrims only listed coaches
// (or an admin's pick); the ruleset may keep coaches out (valve online).

#include "readyup/plugin_api.h"

#include <cstdint>
#include <string>
#include <vector>

namespace readyup {

// `.coach [ct|t|team1|team2]` / `.uncoach` (game thread). `text` is the whole line.
void CoachChatCommand(uint64_t steamid64, const std::string& playerName, const std::string& text, int slot);

// `ru match coach <player> <team>` / `ru match uncoach <player>` (game thread; the caller
// checked the admin rights). The reply for the caller; *announce: a line for everyone ("" = none).
std::string CoachAdminCommand(const std::string& sub, const std::vector<std::string>& args, std::string* announce);

// on_tick (game thread): applies / re-applies coaching, sv_coaching_enabled, hints.
void CoachTick(double now);

// Core events (game thread): disconnects, team changes, map start.
void CoachOnEvent(const ru_event* e);

// A coach an admin assigned (or a scrim coach): the whitelist lets them stay. Thread-safe.
bool CoachAllowedOnServer(uint64_t steamid64);

// `ru match state` lines ("coaches: none" when nobody coaches). Thread-safe.
std::vector<std::string> CoachStateLines();

}  // namespace readyup
