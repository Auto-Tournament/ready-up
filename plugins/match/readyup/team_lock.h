#pragma once
// Team lock (modes.cpp MaybeForceRosterTeamsLocked): whether a roster player has to be moved to
// their team's side. Engine-free, ctest `match_team_lock`.
//
// The core's cached team (ListHumans().team) comes from log lines and player_team events. A map
// load resets it to 0 while the players stay on their side, and moving a player to the team they
// are already on is a no-op in CS2 (CCSPlayerController::ChangeTeam returns early, no event). So
// the cache can say "no team" for a whole map while the player plays CT, and the lock moved them
// again every 900 ms (2026-10-17, a live map: 260 "moved BBro to CT" lines in 2 min). The
// engine's team (m_iTeamNum, read this frame) wins whenever it is known.
#include <optional>

namespace readyup {

// cachedTeam / target / *engineTeam: 0 none, 1 spectators, 2 T, 3 CT.
inline bool TeamMoveNeeded(int cachedTeam, std::optional<int> engineTeam, int target) {
  const int current = engineTeam ? *engineTeam : cachedTeam;
  return current != target;
}

}  // namespace readyup
