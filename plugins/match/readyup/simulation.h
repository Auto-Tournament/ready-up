#pragma once

// Simulation mode, the engine side (simulation_rules.h has the rules and the commands).
//
// A loaded match with `simulation: true` (never a scrim) is played by bots:
//   - each map starts clean (bot_kick, bot_quota 0) once the map is loaded, then one bot per
//     roster player is added (`bot_join_team`, `bot_quota` one step at a time); a bot that goes
//     missing is added again;
//   - every bot plays as a roster player of the team on its side: the ready gate counts that
//     player as connected, it "types .ready" 1.5-3.5 s after it joined (player_ready webhook),
//     `player_connect` / `player_disconnect` webhooks go out for it, and the stats and round
//     events carry its SteamID64 and name (match_events.cpp SteamForSlot);
//   - while a map is live, `sv_cheats 1` + `host_timescale <simulation_timescale>` (not at 1x;
//     warmup, knife round and the time between maps run in real time, because Ready Up's own
//     timers are wall-clock), sent again at every round start;
//   - when the match is gone, the bots leave and CS2's bot defaults come back.
// Knife rounds are cut to 30 s and a bots-only knife winner keeps its side after 3 s (modes.cpp),
// as with the dev bot flags.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

namespace readyup {

// A match with `simulation: true` is loaded (not a scrim). Any thread.
bool SimulationActive();

// Game thread, every tick (match_plugin.cpp).
void SimulationTick(double now);
// RU_EVENT_MAP_START: the bots start over on the new map.
void SimulationOnMapStart(double now);
// A round started: host_timescale again while live (a cfg may have reset it).
void SimulationOnRoundStart();

// Roster SteamID64s a bot plays as right now (the ready gate counts them as connected). Any thread.
void SimulationAddConnected(std::unordered_set<uint64_t>* connected);

struct SimulatedPlayer {
  uint64_t steamid64 = 0;
  std::string name;
};
// The roster player the bot in engine slot `slot` plays as, or nullopt (not a simulated match, not
// a bot, or an anonymous filler bot). Any thread.
std::optional<SimulatedPlayer> SimulationPlayerForSlot(int slot);

}  // namespace readyup
