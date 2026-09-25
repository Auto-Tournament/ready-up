#pragma once

// readyup-essentials pure logic (ctest `essentials_rules`): the admins list (admins.json), finding
// a player by a name fragment, when map commands are refused and the default map per mode
// (default_maps.json). No engine calls.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace essentials {

struct Admin {
  uint64_t steamid64 = 0;
  std::string name;
};

// admins.json: {"version": 1, "admins": [{"steamid64": "7656...", "name": "..."}]} (a plain list
// of SteamID64 strings works too). Duplicates and non-SteamIDs are dropped.
std::vector<Admin> ParseAdmins(const std::string& json, bool* ok);
std::string AdminsJson(const std::vector<Admin>& admins);
bool IsAdmin(const std::vector<Admin>& admins, uint64_t steamid64);
bool AddAdmin(std::vector<Admin>* admins, uint64_t steamid64, const std::string& name);  // false: already one
bool RemoveAdmin(std::vector<Admin>* admins, uint64_t steamid64);                        // false: was not one

// "7656119..." (17 digits) -> the id, else 0.
uint64_t ParseSteamId64(const std::string& s);

struct Player {
  uint64_t steamid64 = 0;
  std::string name;
};
// The one connected player whose name contains `fragment` (case-insensitive; an exact name wins).
// False and *err when none or several match.
bool FindPlayer(const std::vector<Player>& players, const std::string& fragment, Player* out, std::string* err);

// What a player typed after `.ru map change`: a Steam Workshop link
// ("https://steamcommunity.com/sharedfiles/filedetails/?id=3793104017&searchtext=...", with or
// without https / www) becomes its id ("3793104017"); anything else is returned as it is (a map
// name, a workshop id, ws:<id>, workshop/<id>[/name]).
std::string MapArgToEntry(const std::string& arg);

// Map commands (change / reload / restart) are refused in these match plugin modes (a knife round
// or a live map) unless the admin adds `force`.
bool MapCommandBlocked(const std::string& ruMode);

// ---- default maps per mode (default_maps.json, readyup.essentials.v1 default_map) ----------------
//
// {"version": 1, "maps": {"ffa": "aim_map", "tdm": "de_dust2", "practice": "workshop/3084291314"}}
// Keys are mode names ([a-z0-9_]{1,32}: ffa, tdm, practice, warmup, retakes, ... whatever a plugin
// asks for); values are map entries as `ru map change` takes them (a Workshop link is stored as its
// id). Bad keys / values are dropped.
using DefaultMaps = std::map<std::string, std::string>;

// The modes `ru map defaults` always lists (set or not).
const std::vector<std::string>& KnownDefaultMapModes();
bool ValidModeName(const std::string& mode);
DefaultMaps ParseDefaultMaps(const std::string& json, bool* ok);
std::string DefaultMapsJson(const DefaultMaps& maps);
// The entry for `mode` (lower-cased), "" when none.
std::string DefaultMapFor(const DefaultMaps& maps, const std::string& mode);
// Sets `mode` to `mapArg` (anything MapArgToEntry takes); "" or "clear" removes it. False and
// *err for a bad mode name or map.
bool SetDefaultMap(DefaultMaps* maps, const std::string& mode, const std::string& mapArg, std::string* err);

// Center-screen panel while the server downloads a Workshop map: a `segments`-wide bar of block
// characters, the percentage and MB. total 0 = Steam does not know the size yet. `name` is
// HTML-escaped. (CS2's center panel runs no script; the plugin resends this ~10x a second.)
std::string DownloadPanelHtml(const std::string& name, uint64_t downloaded, uint64_t total, int segments = 30);

}  // namespace essentials
