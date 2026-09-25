#pragma once
// Match facts for the fleet link (fleet_bridge.h, docs/FLEET.md §8.1): round start / end,
// halftime, overtime, knife result, side pick, gg / forfeit, demo events, round backups.
//
// Emit() may be called from any thread and with any match lock held (modes, match_events,
// stats): it only appends to a small queue under its own mutex. fleet_bridge drains the queue on
// the game thread (next tick), builds the MatchState once and turns each signal into an
// `event.<type>` with the state patch. Nothing is queued while the bridge has no platform
// assignment (scrims and `ru match load` matches are never reported).
#include "readyup/status_snapshot.h"

#include <string>
#include <vector>

namespace readyup::signals {

struct Signal {
  std::string type;   // event name without the "event." prefix
  status::Json data;  // the event's `data`
  int round = -1;     // the event's `round` (-1 = the current round from MatchState)
  int map_number = -1;  // -1 = the current map
};

void Emit(const char* type, status::Json data, int round = -1, int mapNumber = -1);
// fleet_bridge: on while a platform assignment is active. Off drops what is queued.
void SetEnabled(bool on);
bool Enabled();
std::vector<Signal> Take();

}  // namespace readyup::signals
