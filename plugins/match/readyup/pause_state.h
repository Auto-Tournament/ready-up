#pragma once

#include "readyup/webhook.h"

#include <cstdint>
#include <optional>

namespace readyup {

struct PauseSnapshot {
  bool paused = false;
  bool team1_ready_to_unpause = false;
  bool team2_ready_to_unpause = false;
};

// Marks paused (and resets unpause readiness).
void PauseStateOnPaused();

// Marks unpaused (clears state).
void PauseStateOnUnpaused();

// Marks a team as having requested unpause. Returns current snapshot.
PauseSnapshot PauseStateRequestUnpause(WebhookTeam team);

// Returns current snapshot.
PauseSnapshot PauseStateGet();

// Returns seconds since pause started (0 if not paused).
int PauseStatePauseDurationSeconds();

// Plugin reload (reload_state.cpp). startTicks: the pause start as steady_clock ticks.
void PauseStateSave(PauseSnapshot* snap, long long* startTicks);
void PauseStateRestore(const PauseSnapshot& snap, long long startTicks);

}  // namespace readyup

