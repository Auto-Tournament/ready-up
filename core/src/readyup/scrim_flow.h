#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace readyup {

// Pickup/scrim flow (no match config loaded):
//
//   idle --(human on CT/T, scrim auto on)--> scrim_warmup
//   scrim_warmup --(no human on CT/T for 5s)--> idle
//   scrim_warmup --(all humans on CT/T ready + both sides populated)--> 5s countdown
//   countdown --(someone .ur / leaves / side empties)--> scrim_warmup (cancelled)
//   countdown --(expires)--> scrim match context created -> match_warmup, then
//       scrim_knife=1 (default): knife round (log-driven, see modes.h KnifePhase)
//           -> winner .stay/.switch (timeout: stay) -> live.cfg + restart
//       scrim_knife=0: go-live right away (live.cfg + mp_warmup_end + mp_restartgame 1)
//   match_warmup (go-live pending) --(Round_Start log line)--> match_live
//   scrim match, no humans connected for 60s --> idle (scrim ended)
//
// `.ru idle` pauses the idle -> scrim_warmup auto-entry until `.ru scrim` or
// the next map change.

struct ScrimRoster {
  // Humans on CT/T: steamid64 -> cs team num (2=T, 3=CT).
  std::unordered_map<uint64_t, int> teamNum;
  // Connected humans not on CT/T (spectators/unassigned).
  std::unordered_set<uint64_t> spectators;
  // dev_bots_ready only: bot pseudo-id (see IsDevBotId) -> cs team num.
  // Bots are always READY and only count toward the "both sides populated"
  // check; they are never written into the match context, so they never
  // reach webhooks, the DB or persisted state.
  std::unordered_map<uint64_t, int> devBots;
};

struct ScrimCounts {
  int humansCt = 0;
  int humansT = 0;
  int devBotsCt = 0;
  int devBotsT = 0;
  int ready = 0;  // ready humans on CT/T
  int total = 0;  // humans on CT/T
  bool bothSides = false;
  bool allReady = false;  // total > 0 && ready == total
};

// Current teams from the log-derived human table (engine netvars when events
// are available) plus dev_bots_ready bots.
ScrimRoster BuildScrimRoster();
ScrimCounts CountScrimRoster(const ScrimRoster& roster);

// Creates the scrim match context from the current teams and goes live.
// Returns true if a scrim was started.
bool MaybeStartScrimIfAllReady(const ScrimRoster& roster);

// GameFrame thread (after modes Tick()).
void ScrimTick();

// `.ru idle` -> false, `.ru scrim` / map change -> true.
void ScrimSetAutoEnabled(bool enabled);
bool ScrimAutoEnabled();

// Seconds left on the all-ready countdown, or -1 when none is running.
int ScrimCountdownSecondsLeft();

// Router hook after a `.r`/`.ur` change: logs a `state:` line right away.
void ScrimNoteReadyChanged();

// One-line structured state log: `[ReadyUp] state: mode=... reason=<reason>`.
void EmitStateLog(const char* reason);

// Multi-line human-readable report for `.ru state` / `ru state`.
std::vector<std::string> BuildStateReport();

}  // namespace readyup
