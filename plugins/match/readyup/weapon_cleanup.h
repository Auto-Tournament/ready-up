#pragma once

// Warmup weapon cleanup: warmup is emulated and can run for hours, so dropped weapons must not
// pile up on the ground (every one is a networked entity). Two layers:
//  - cvars: nothing drops on death (kWarmupNoDropCmds), sent with the warmup rules whether or
//    not ru_cfg_exec_enable runs warmup.cfg; the go-live paths put CS2's defaults back
//    (kLiveDropCmds; live.cfg / esports_live.cfg set the same values).
//  - sweep: in idle, scrim warmup and match warmup (not once go-live is pending), a weapon_*
//    entity with no owner for kWeaponCleanupGraceSeconds is removed (ru_api entity_remove, core
//    API 1.5). Covers G-drops, which weapon_auto_cleanup_time only removes when no player is
//    near. The C4 is left alone. readyup.cfg / match.cfg `warmup_weapon_cleanup=0` turns the
//    sweep off. Log lines: `weapon-cleanup: ...` (debug only, plus one line if unavailable).

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

struct ru_api;  // readyup/plugin_api.h

namespace readyup {

// Warmup: no death drops. mp_death_drop_gun / _grenade / _defuser / _taser exist on CS2 1.41.8
// (live.cfg, esports_live.cfg); the knife never drops on death. The cleanup cvars are the
// fallback when the sweep is unavailable: G-drops go after a second when no player is near, and
// never more than 10 lie around (the oldest goes first).
inline constexpr const char* kWarmupNoDropCmds[] = {
    "mp_death_drop_gun 0",
    "mp_death_drop_grenade 0",
    "mp_death_drop_defuser 0",
    "mp_death_drop_taser 0",
    "weapon_auto_cleanup_time 1",
    "weapon_max_before_cleanup 10",
};
// Go-live without a cfg (ru_cfg_exec_enable 0): CS2's defaults, as live.cfg sets them.
inline constexpr const char* kLiveDropCmds[] = {
    "mp_death_drop_gun 1",
    "mp_death_drop_grenade 2",
    "mp_death_drop_defuser 1",
    "mp_death_drop_taser 1",
    "weapon_auto_cleanup_time 0",
    "weapon_max_before_cleanup 0",
};

// A weapon on the ground this long (seconds, seen unowned on two sweeps) is removed.
constexpr double kWeaponCleanupGraceSeconds = 2.0;

// Pure (ctest `match_rules`): sweep in this ru mode string? Not once go-live is pending (the
// restart that follows brings the map's own weapons back).
inline bool WeaponCleanupActive(const char* ruMode, bool goLivePending, bool enabled) {
  if (!enabled || goLivePending || !ruMode) return false;
  const std::string m = ruMode;
  return m == "idle" || m == "scrim_warmup" || m == "match_warmup";
}

// Pure: a designer name the sweep removes when it has no owner (every weapon but the C4).
inline bool WeaponCleanupClass(const char* designer) {
  if (!designer || std::strncmp(designer, "weapon_", 7) != 0) return false;
  return std::strcmp(designer, "weapon_c4") != 0;
}

// Pure: grace tracking by entity handle. Unowned() returns true once the same handle was seen
// unowned for kWeaponCleanupGraceSeconds; Owned() (picked up) starts it over. Prune() forgets
// handles not seen for a while (removed, or reused with a new serial).
class DroppedWeaponTracker {
 public:
  bool Unowned(uint32_t handle, double now) {
    auto it = seen_.find(handle);
    if (it == seen_.end()) {
      seen_[handle] = {now, now};
      return false;
    }
    it->second.last = now;
    return now - it->second.first >= kWeaponCleanupGraceSeconds;
  }
  void Owned(uint32_t handle) { seen_.erase(handle); }
  void Forget(uint32_t handle) { seen_.erase(handle); }
  void Prune(double now) {
    for (auto it = seen_.begin(); it != seen_.end();) {
      if (now - it->second.last > 2 * kWeaponCleanupGraceSeconds) it = seen_.erase(it);
      else ++it;
    }
  }
  void Clear() { seen_.clear(); }
  size_t Size() const { return seen_.size(); }

 private:
  struct Seen {
    double first;
    double last;
  };
  std::unordered_map<uint32_t, Seen> seen_;
};

// readyup_plugin_load.
void WeaponCleanupInstall(const ru_api* api);
// on_tick.
void WeaponCleanupTick(double now);
// `ru selftest` line (any thread).
std::string WeaponCleanupStatus();

}  // namespace readyup
