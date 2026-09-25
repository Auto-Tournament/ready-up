#pragma once

// readyup-whitelist pure logic (ctest `whitelist_rules`): the saved state and who gets kicked.
// No engine calls; whitelist_plugin.cpp is the engine side.

#include <cstdint>
#include <set>
#include <string>

namespace whitelist {

struct State {
  bool enabled = false;
  std::set<uint64_t> steamids;
};

// whitelist.json: {"version": 1, "enabled": true, "steamids": ["7656119...", ...]}. Missing file
// (""): default state. Bad JSON: default state and *ok false. SteamIDs are strings; anything that
// is not a SteamID64 string is skipped.
State ParseState(const std::string& json, bool* ok);
std::string StateJson(const State& s);

// "7656119..." (17 digits) -> the id, else 0.
uint64_t ParseSteamId64(const std::string& s);

// While a match is loaded its roster decides who may join (the match plugin kicks), so the
// whitelist stands down in these ru_modes.
bool MatchOwnsRoster(const std::string& ruMode);

// Kick a connected player? Never bots (steamid 0), admins, or anyone while it is off or a match
// owns the roster.
bool ShouldKick(const State& s, const std::string& ruMode, uint64_t steamid64, bool isBot, bool isAdmin);

}  // namespace whitelist
