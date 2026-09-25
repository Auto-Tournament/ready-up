#pragma once

// The match half of what used to be the core's log_receiver.cpp (LifecycleImpl): server log lines
// (ru_api subscribe_log_line, game thread, in log order) -> map number / round / score tracking
// (match_state.h), the log-driven knife round, the log fallback of the round lifecycle when
// engine events are not delivered, connect/disconnect webhooks, the welcome screen's team-switch
// trigger and the observed-player list for `ru admins add <name>`.

#include <string>

namespace readyup {

// One server log line (the core already filters out Ready Up's own output).
void MatchObserveLogLine(const std::string& line);

// Reload state: the log-derived counters that are not in MatchState.
struct MatchLogState {
  int mapNumber = 1;
  int roundNumber = 0;
  int team1Score = 0;
  int team2Score = 0;
  std::string currentMap;
};
MatchLogState MatchLogSnapshot();
void MatchLogRestore(const MatchLogState& s);
// Plugin loaded mid-map: seed the current map (ru_api current_map) without a map change.
void MatchLogSeedMap(const std::string& map);

}  // namespace readyup
