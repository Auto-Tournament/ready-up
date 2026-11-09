// readyup-retakes rules: everything that needs no engine (ctest `retakes_rules`).
//
//   - Valve's retake spawns: CS2 ships maps/<map>_retake.txt (A/B, CT/T, "x y z pitch yaw roll")
//     for its retake maps inside pak01_dir.vpk. The plugin reads them from the player's own game
//     install at map start (VpkFind + ParseRetakeFile); nothing of Valve's is shipped.
//   - Team sizes, the queue past max_players, scramble after a T streak, CT winners moving to T.
//   - Spawns and the planter for a round.
//
// The round flow it serves (retakes_plugin.cpp): round_prestart picks teams, round_start puts
// everyone on the site's spawns, round_poststart gives loadouts, freeze end plants the bomb.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace retakes {

// ---- Valve's game data ------------------------------------------------------------------------

// One file in a VPK v2 directory (pak01_dir.vpk).
struct VpkEntry {
  uint16_t archive = 0;  // pak01_<archive>.vpk; kVpkInDir = after the tree in the _dir file
  uint32_t offset = 0;
  uint32_t length = 0;
  std::string preload;   // bytes kept in the tree itself (come first)
};
constexpr uint16_t kVpkInDir = 0x7fff;
constexpr size_t kVpkHeaderV2 = 28;

// The tree size from a VPK v2 header (the first 28 bytes); 0 = not a v2 VPK.
uint32_t VpkTreeSize(const uint8_t* header, size_t n);
// Finds `path` ("maps/de_mirage_retake.txt", lower case) in the tree (the bytes after the header).
bool VpkFind(const uint8_t* tree, size_t n, const std::string& path, VpkEntry* out);

struct Spawn {
  float pos[3] = {0, 0, 0};
  float ang[3] = {0, 0, 0};  // pitch yaw roll
};
struct SiteSpawns {
  std::vector<Spawn> ct, t;
};
// Valve's retake spawns: site[0] = A, site[1] = B.
struct MapSpawns {
  SiteSpawns site[2];
  bool Usable() const;  // both sites have CT and T spawns
};
// Parses a maps/<map>_retake.txt (KeyValues: "Retake" > "Spawns" > "CT"|"T" > "0"|"1" > "n" "x y z
// p y r"). false + *err when it is not one.
bool ParseRetakeFile(const std::string& text, MapSpawns* out, std::string* err);

// ---- teams ------------------------------------------------------------------------------------

// Terrorists for `players` on the server: round(0.45 n), at least 1 with two or more players;
// half at multiples of 10 (5v5, 10v10).
int TargetTerrorists(int players);

enum Team { kSpectator = 1, kT = 2, kCT = 3 };

struct Player {
  uint64_t id = 0;     // SteamID64 (bots: their slot + 1)
  int team = kSpectator;
  int score = 0;       // this round: kill 50, assist 25, defuse 50
  bool queued = false; // waiting in spectator for a free place
  int order = 0;       // when they joined the queue (lower first)
};

// A shuffle the caller seeds (tests pass a fixed one).
using Shuffle = std::function<void(std::vector<size_t>&)>;

struct Teams {
  // Who plays T / CT next round (ids), and who waits.
  std::vector<uint64_t> t, ct, queued;
  bool scrambled = false;
};

struct Streak {
  int tWins = 0;  // rounds the Ts have won in a row
};

// The next round's teams, from the last one: the queue fills free places (up to `maxPlayers`,
// oldest first), then
//   - Ts won: streak + 1; at `scrambleAfter` (0 = never) everyone is shuffled into new teams;
//   - CTs won: the streak resets; the best CTs of the round (score > 0, highest first) become T,
//     topped up with other CTs, then with the best of the old Ts;
// and the counts are balanced to TargetTerrorists. `lastWinner`: kT, kCT, or 0 (no round yet).
Teams NextTeams(const std::vector<Player>& players, int lastWinner, int maxPlayers, int scrambleAfter,
                Streak* streak, const Shuffle& shuffle);

// ---- the round --------------------------------------------------------------------------------

struct Placement {
  uint64_t id = 0;
  int spawn = -1;  // index into the site's CT or T list; -1 = no spawn left (left where it is)
};
struct RoundSpawns {
  std::vector<Placement> t, ct;
  uint64_t planter = 0;  // the T who has the bomb planted at their spawn; 0 = no T
};

// Spawns for one site: each player a different spawn of their team, in shuffled order. The planter
// is the first T and gets a spawn from `plantable` (indices into the T list inside the bombsite)
// when there is one, any T spawn otherwise.
RoundSpawns PlaceRound(const SiteSpawns& site, const std::vector<uint64_t>& t, const std::vector<uint64_t>& ct,
                       const std::vector<int>& plantable, const Shuffle& shuffle);

// ---- loadouts (.guns) -------------------------------------------------------------------------

// The full-buy guns a player can pick: side kT / kCT for a rifle, 0 for the AWP (one per team).
struct Gun {
  const char* words;  // what `.guns <word>` accepts, space separated
  const char* cls;
  const char* name;
  int side;
};
const std::vector<Gun>& Guns();  // T: AK-47, SG 553; CT: M4A4, M4A1-S, AUG; the AWP

struct Loadout {
  std::string t = "weapon_ak47", ct = "weapon_m4a1_silencer";
  bool awp = false;  // in the draw for the team's AWP
};
// `.guns <word>` ("ak", "m4s", "awp" toggles, ...): changes `l`; false when the word is no gun.
bool PickGun(const std::string& word, Loadout* l);
// guns.txt: one "steamid64 t-weapon ct-weapon awp(0|1)" per line; bad lines are skipped.
std::map<uint64_t, Loadout> ParseLoadouts(const std::string& text);
std::string FormatLoadouts(const std::map<uint64_t, Loadout>& all);

}  // namespace retakes
