#pragma once

// `.skins reload` (chat; `.ru skins reload` reaches it too): a player re-reads their own loadout
// (e.g. after the platform's store changed it) and gets it re-applied: gloves and agent at once,
// the weapons they hold repainted. Only while nothing is live (idle, practice, scrim or match
// warmup); refused during knife rounds, live maps and postgame, whatever the plugin config. Pure
// rules here (ctest `skins_reload`); the command is in skins_plugin.cpp.

#include <string>

namespace skins {

// ru_mode of readyup.match.v1 ("" = no match plugin loaded: nothing can be live).
inline bool SkinsReloadAllowed(const std::string& ruMode) {
  return ruMode.empty() || ruMode == "idle" || ruMode == "practice" || ruMode == "scrim_warmup" ||
         ruMode == "match_warmup";
}

// One reload per player per this many seconds (each one re-reads the file / asks the platform).
constexpr double kSkinsReloadCooldownSeconds = 10.0;
inline bool SkinsReloadCooldownOk(double now, double last) {
  return last < 0 || now - last >= kSkinsReloadCooldownSeconds;
}

}  // namespace skins
