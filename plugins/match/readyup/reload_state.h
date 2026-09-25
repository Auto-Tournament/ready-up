#pragma once

// What survives `ru plugin reload match` (docs/ARCHITECTURE.md §4, "Match-specific").
//
// readyup_plugin_unload saves one JSON document with ru_api stash_put("state"); the next
// readyup_plugin_load reads it back before anything runs. It holds:
//   - the loaded match (the full match context: teams, roster, maps, sides, cvars, admins),
//     the mode (warmup / knife / live / postgame / scrim / practice) and every ready state,
//   - map number, round, scores (log-derived and the stats model's team1/team2 score), the
//     per-map stats model and event totals, halftime / overtime counters, series wins,
//   - the knife round (phase, winner, pick deadline, deaths / HP so far), pause state,
//   - runtime settings: webhook / heartbeat / admins URLs, match token, ru_warmup_* rules,
//     ru_cfg_exec_enable, ru_dev_bots_scrim, demo settings, series-end kick delays,
//   - undelivered webhook events, the demo recording in progress, a pending GOTV-flush stop,
//     demo uploads the unload interrupted (restarted), the pending postgame step (next map /
//     kick / unload; rescheduled with the time left), `.ru idle`.
// Not kept: per-player UI throttles, the scrim countdown (it restarts), the round in progress in
// the stats model (starts over empty), log lines / events that arrived while no image was
// loaded (one frame). The stash lives in core memory only: a server restart uses the
// match state persisted in state.json (local_store.h, match_recovery.h) as before.

namespace readyup {

// readyup_plugin_unload (game thread, after the worker threads stopped).
void ReloadStateSave();
// readyup_plugin_load (game thread). True if a previous image's state was restored.
bool ReloadStateRestore();

}  // namespace readyup
