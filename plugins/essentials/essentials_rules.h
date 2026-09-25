#pragma once

// readyup-essentials pure logic (ctest `essentials_rules`): the admins list (admins.json), finding
// a player by a name fragment, and when map commands are refused. No engine calls.

#include <cstdint>
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

}  // namespace essentials
