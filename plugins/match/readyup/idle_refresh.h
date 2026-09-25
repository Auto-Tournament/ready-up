#pragma once

// Idle map refresh. After a day or more of uptime player animations run in slow motion while
// the tick rate is fine (float precision of the engine clock, most likely); a map load fixes it.
// So when the server has sat on one map for `idle_map_refresh_hours` (readyup.cfg / match.cfg,
// default 12, 0 = off) with no match loaded and no human connected, it loads the same map again
// (`ru map reload`'s entry: workshop maps by their id). Loading a match always changes map too
// (match_console.cpp ApplyLoadedMatch).
//
// Log lines: `idle-refresh: ...`.

namespace readyup {

// Pure decision (ctest `match_idle_refresh`). Times in seconds (monotonic). lastMapLoad: the
// last map start this plugin saw (its own load time when it loaded mid-map); lastAttempt: the
// last refresh it issued (< 0 = none), retried at most every kIdleRefreshRetrySeconds.
constexpr double kIdleRefreshRetrySeconds = 600.0;
inline bool IdleRefreshDue(double now, double lastMapLoad, double lastAttempt, int hours, bool matchLoaded,
                           int humans) {
  if (hours <= 0 || matchLoaded || humans > 0 || lastMapLoad < 0) return false;
  if (now - lastMapLoad < hours * 3600.0) return false;
  return lastAttempt < 0 || now - lastAttempt >= kIdleRefreshRetrySeconds;
}

// readyup_plugin_load, and every RU_EVENT_MAP_START.
void IdleRefreshOnMapStart(double now);
// on_frame (also runs while the server does not simulate).
void IdleRefreshFrame(double now);

}  // namespace readyup
