#pragma once

// The walkover choice on the HUD (walkover.h), as pure functions so they are tested on their own.

#include "readyup/status_snapshot.h"

#include <cstdint>
#include <string>

namespace readyup {

struct WalkoverView {
  std::string shortName;  // the team that is short
  std::string fullName;   // the team that chooses
  long long secondsLeft = 0;
  bool onFullTeam = false;  // the viewer plays for the full team
  bool onShortTeam = false;
  bool chooser = false;  // the viewer is its owner or a captain
};

// The screen for one viewer: the choice (buttons, cursor forced on: it can't be skipped) for a
// chooser, "your captain chooses" for the rest of the full team, the wait for the short team and
// spectators.
std::string WalkoverSpec(const WalkoverView& v, const std::string& lang);

// Seconds until `untilMs` (unix ms) from `nowMs`, at least 0; -1 when `untilMs` is 0 (none).
long long SecondsUntil(long long untilMs, long long nowMs);

// The fleet `event.walkover_choice` data: who chose, and what.
status::Json WalkoverChoiceData(uint64_t steamid64, const std::string& name, bool take);

}  // namespace readyup
