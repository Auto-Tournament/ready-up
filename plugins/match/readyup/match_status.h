#pragma once

// readyup.match.v1 (core/include/readyup/match_iface.h): the match part of the core's local
// status endpoint. Moved from the core's status_feed.cpp: the summary fields, MatchState
// (docs/FLEET.md §9.1) and update_safe, plus the series / demo bookkeeping they need (fed by the
// match-flow and demo listeners).

#include "readyup/match_iface.h"
#include "readyup/status_snapshot.h"

namespace readyup {

// Load: registers the match-flow / demo listeners.
void MatchStatusInstall();

// readyup.match.v1 get_status (game thread).
int MatchStatusGet(ru_match_status* out);

// The MatchState of the loaded match / scrim as the status endpoint builds it from the match
// flow (null when none is loaded). fleet_bridge overlays it on the platform's config. Game thread.
status::Json MatchStatusStateJson();

// Plugin reload (reload_state.cpp): series score / map results / demo states.
status::Json MatchStatusSnapshotJson();
void MatchStatusRestoreJson(const status::Json& j);

}  // namespace readyup
