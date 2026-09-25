#pragma once

// readyup-midas pure logic (ctest `midas_rules`): the config values and when a weapon is tinted.
// No engine calls; midas_plugin.cpp is the engine side.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

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

// Whole number in [lo, hi] (surrounding spaces allowed); `def` for anything else.
int ParseInt(const std::string& text, int def, int lo, int hi);
// Decimal number in [lo, hi]; `def` for anything else.
float ParseFloat(const std::string& text, float def, float lo, float hi);

// ---- finish: tint or paint kit (through skins.so, readyup.skins.v1) --------------------------

// `finish`: "auto" (the default; "paint" is accepted too): a paint kit through skins.so when it is
// loaded and active, else the tint. "tint": always the render colour. False for anything else.
enum class Finish { kAuto, kTint };
bool ParseFinish(const std::string& text, Finish* out);

// The default `paint_kit`: 1025 "Gold Brick" (items_game am_gold_brick, the MAC-10 finish). An
// anodized multicolour pattern (style 5, not a legacy-model kit), so it lays out on every gun
// instead of being authored for one weapon's UVs (unlike e.g. 921 "Gold Arabesque", AK-47 only).
constexpr int kGoldPaintKit = 1025;

// Weapons the paint kit goes on (weapon_* guns and the Zeus). Knives, grenades, the C4 and the
// other extras (healthshot, shield, ...) keep the tint: they have no paint kits to show.
bool Paintable(const std::string& classname);

// ---- best player ("the best player becomes Midas") -----------------------------------------

// `best_player_stat`: "adr" (default) or "kills". `best_player_when`: "round" (default: every
// round start once `best_player_min_rounds` rounds are played) or "half" (at the start of every
// half after the first: the best of the map so far, for that half).
enum class BestStat { kAdr, kKills };
enum class BestWhen { kRound, kHalf };
bool ParseBestStat(const std::string& text, BestStat* out);
bool ParseBestWhen(const std::string& text, BestWhen* out);

// One connected player's totals on this map (the match plugin's readyup.match.v1 map_stats).
struct PlayerTotals {
  uint64_t steamid64 = 0;
  int kills = 0;
  int deaths = 0;
  int damage = 0;
  int rounds = 0;  // rounds played
};

// Average damage per round played (0 without a round).
double Adr(const PlayerTotals& p);

// The rule may pick a Midas: `best_player` on, the match plugin's stats recording, not the valve
// ruleset, and a scrim, or a real match with `best_player_in_matches=1` (off by default).
bool BestPlayerAllowed(bool enabled, bool inMatches, bool live, bool scrim, const std::string& ruleset);

// Time to pick at this round start. round: `roundsPlayed` >= max(1, minRounds). half: `half` >= 2
// and no pick for that half yet (`lastPickHalf`).
bool PickNow(BestWhen when, int roundsPlayed, int minRounds, int half, int lastPickHalf);

// The best player: highest `stat` (ADR or kills); ties go to the other stat, then fewer deaths,
// then `current` (the Midas stays), then the lowest SteamID64. Players without a round played,
// and a best "score" of nothing (no kill and no damage), give 0: nobody.
uint64_t PickBest(const std::vector<PlayerTotals>& players, BestStat stat, uint64_t current);

}  // namespace midas
