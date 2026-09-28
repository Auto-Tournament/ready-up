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

// Who gets skins.so's per-player gold paint (new weapons painted when they are created): nobody
// while inactive or with finish=tint, else midas_steamids plus the best-player Midas.
std::set<uint64_t> PaintOverrideSet(bool active, Finish finish, const std::set<uint64_t>& midas, uint64_t best);

// The default `paint_kit`: 1025 "Gold Brick" (items_game am_gold_brick, the MAC-10 finish). An
// anodized multicolour pattern (style 5, not a legacy-model kit), so it lays out on every gun
// instead of being authored for one weapon's UVs (unlike e.g. 921 "Gold Arabesque", AK-47 only).
constexpr int kGoldPaintKit = 1025;

// Weapons the paint kit goes on (weapon_* guns and the Zeus). Knives, grenades, the C4 and the
// other extras (healthshot, shield, ...) keep the tint: they have no paint kits to show.
bool Paintable(const std::string& classname);

// ---- StatTrak (`stattrak=1`): the Midas player's kills on this map on their Midas weapons ------

// Weapons that show a StatTrak counter: the paintable ones (guns and knives), not the Zeus.
bool StatTrakable(const std::string& classname);

// A player_death that counts as a kill for the StatTrak counter (like the match plugin's stats):
// a real attacker slot, not a suicide, not a team kill (teams 2 / 3; unknown teams count).
bool CountsAsKill(int attackerSlot, int victimSlot, int attackerTeam, int victimTeam);

// The counter: the match plugin's kills while its stats record this map and list the player,
// else Midas's own count of player_death kills since the map started.
int StatTrakKills(bool statsLive, bool statsHavePlayer, int statsKills, int ownKills);

// "kill eater" is an integer attribute stored in the attribute's float bits (as skins.so writes
// it): the float whose bits are `kills` (negative -> 0).
float KillEaterBits(int kills);

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

// ---- who is Midas: midas_steamids + given (`.ru midas give`) + the best player ----------------

// Why a player is Midas (bits). 0 = not Midas.
enum MidasWhy : unsigned { kWhyConfig = 1u, kWhyGiven = 2u, kWhyBest = 4u };
// The reasons `sid` is Midas: on midas_steamids, given by an admin, the best-player Midas (`best`,
// 0 = none). Nothing while inactive or for SteamID64 0.
unsigned MidasReasons(bool active, const std::set<uint64_t>& config, const std::set<uint64_t>& given, uint64_t best,
                      uint64_t sid);
// Everyone who is Midas (active): config + given + best; never 0.
std::set<uint64_t> EffectiveMidas(bool active, const std::set<uint64_t>& config, const std::set<uint64_t>& given,
                                  uint64_t best);
// "midas_steamids, given by an admin, best player" (the set bits, in that order; "" for 0).
std::string DescribeReasons(unsigned why);

// `.ru midas give`: toggles `sid` in the given set. True = added, false = it was given, now removed.
bool ToggleGiven(std::set<uint64_t>* given, uint64_t sid);

// plugins/midas/given.txt: one SteamID64 per line ('#' comments and anything else skipped).
std::set<uint64_t> ParseGivenFile(const std::string& text);
std::string FormatGivenFile(const std::set<uint64_t>& given);

// Player name lookup for admin commands (`.ru midas give <player>`), like the essentials plugin's
// map-name resolver: 1. exact, 2. case-insensitive exact, 3. case-insensitive prefix (several: the
// shortest name if it is the only one that short), 4. substring (>= 2 chars; names where a word
// starts with it first, then the shortest likewise), 5. the smallest edit distance if it is at most
// max(1, len/3) and not shared. `index` is the match in `names` (-1 = none); a tie leaves index -1
// and lists the tied candidates (at most 5) in `ambiguous`.
struct PlayerMatch {
  int index = -1;
  std::vector<int> ambiguous;
};
PlayerMatch ResolvePlayerName(const std::string& query, const std::vector<std::string>& names);

// ---- gold equipment in flight / planted ----------------------------------------------------------

// The item a thrown projectile / planted bomb stands for, by its classname: its item definition
// index (models.txt is keyed by it) and the weapon classname (model_<classname> in midas.cfg).
// molotov_projectile is shared by the molotov (46) and the incendiary (48): `incendiary` picks.
// 0 / "" for anything else.
struct EquipmentItem {
  int defindex = 0;
  const char* classname = "";
};
EquipmentItem EquipmentItemFor(const std::string& entityClass, bool incendiary);

}  // namespace midas
