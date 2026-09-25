#pragma once

// The match half of what used to be the core's game_events.cpp: engine game events
// (ru_api subscribe_game_event) -> round lifecycle (warmup -> live, halftime / overtime side
// swaps, knife round end, round_end webhooks, map-end detection), per-player stats
// (match_stats.h) and damage totals for the damage tiebreak.
//
// Timing: stats events (player_death, player_hurt, ...) are handled synchronously inside the
// engine's dispatch. round_start / round_end are copied and handled on the same frame's tick,
// after the core delivered that frame's log lines: the log-derived scores (SFUI notices) the
// round end webhook reports are then up to date, whichever order the engine logged and fired
// them in.

#include "readyup/plugin_api.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace readyup {

// Load: subscribes the raw engine events it needs.
void MatchEventsInstall(const ru_api* api);
// on_tick: runs the round events queued this frame (after the core delivered log lines).
void MatchEventsTick();

// True once engine events are delivered (ru_api feature_state "events_live"): they then drive
// the round lifecycle, otherwise log lines do. Any thread (cached per frame off the game thread).
bool GameEventsListenerInstalled();

// Team of a player slot (2 = T, 3 = CT): the controller's m_iTeamNum when engine events are
// live, else the log-derived team. Game thread.
std::optional<int> GetCsTeamNumForSlot(int slot);

// Total roster-team damage on the current map, {team1, team2} (player_hurt dmg_health).
std::pair<int, int> GetRosterTeamDamageTotals();

// Engine slot of a connected player (ru_api slot_for_steamid), or nullopt. Game thread.
std::optional<int> GameEventsSlotForSteam(uint64_t steamid64);

// Reload state (reload_state.cpp): the per-map round counters that are not in MatchState.
struct MatchEventsState {
  int roundNumber = 0;
  int lastMapNumber = 0;
  int swapCount = 0;
  int lastHalfStartTotal = -1;
  int lastOvertimeNumber = 0;
  // Event-accumulated totals (round_end webhook payload, damage tiebreak).
  struct Totals {
    uint64_t steamid64 = 0;
    std::string name;
    int team = 0;  // WebhookTeam
    int kills = 0, deaths = 0, assists = 0, headshot_kills = 0, damage = 0;
  };
  std::vector<Totals> players;
};
MatchEventsState MatchEventsSnapshot();
void MatchEventsRestore(const MatchEventsState& s);

}  // namespace readyup
