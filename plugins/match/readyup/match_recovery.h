#pragma once

// Server restart recovery: a match that was loaded when the server went down comes back.
//
// state.json (local_store.h) keeps the match config (persisted_match_state.h) and a progress
// record (round_restore_rules.h Progress): the map number, whether that map was in warmup, live
// or over, the series score (maps won) and, while live, the stats model of the map and the event
// counters. It is rewritten at every round end, go-live, map end and restore (NoteProgress).
//
// On a fresh plugin load (not `ru plugin reload match`, which keeps everything in memory) the
// match context, series score and map number come back (PlanRecovery decides which map), the
// server changes to that map if it is on another one, and then:
//   - live map: the newest CS2 round backup of that map is restored (fleet_bridge.h
//     RestoreRoundFromLocalBackup: rounds after it voided in the stats), the stats model continues
//     from the record, the match waits paused until every roster player is back and ready
//     (modes.h recovery gate), and `recover_requested` + `backup_loaded` are sent;
//   - warmup / the map after a finished one: ordinary match warmup on that map;
//   - the series was over: nothing is recovered (the record is cleared).
//
// Log lines: `recovery: ...`.

namespace readyup::match_recovery {

// Plugin load without a previous image: runs the recovery on the game thread shortly after.
void TryRecoverAsync();
// RU_EVENT_MAP_START: a recovery waiting for the match map continues. Game thread.
void OnMapStart();
// Any thread: save the progress record on the next frame (coalesced).
void NoteProgress();

}  // namespace readyup::match_recovery
