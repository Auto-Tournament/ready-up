#pragma once

// readyup-midas pure logic (ctest `midas_rules`): the config values and when a weapon is tinted.
// No engine calls; midas_plugin.cpp is the engine side.

#include <cstdint>
#include <set>
#include <string>

namespace midas {

struct Rgba {
  uint8_t r = 255, g = 200, b = 40, a = 255;
  bool operator==(const Rgba& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
  bool operator!=(const Rgba& o) const { return !(*this == o); }
};

// The gold tint (the default `color`).
constexpr Rgba kGold{};
// What the engine uses when nothing tints an entity (restored on unload / going inert).
constexpr Rgba kWhite{255, 255, 255, 255};

// `midas_steamids`: SteamID64s separated by commas, spaces or semicolons. Anything that is not a
// SteamID64 (17 digits starting with 7656119) is skipped and counted in *bad.
std::set<uint64_t> ParseSteamIds(const std::string& text, int* bad);

// `color`: "r,g,b" or "r,g,b,a", each 0..255. False (and *out unchanged) for anything else.
bool ParseColor(const std::string& text, Rgba* out);

// "1"/"true"/"yes"/"on" (any case) -> true; "0"/"false"/"no"/"off" -> false; else `def`.
bool ParseBool(const std::string& text, bool def);

// Midas is active when it is enabled and the ruleset is not "valve" (Valve's rulebook: nothing
// that changes how items look; case-insensitive).
bool Active(bool enabled, const std::string& ruleset);

// Weapons of `owner` get the tint when Midas is active and the owner is on the list.
bool ShouldTint(bool active, const std::set<uint64_t>& midas, uint64_t owner);

}  // namespace midas
