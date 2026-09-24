#pragma once

// readyup-skins internals. The plugin talks to the core only through ru_api
// (core/include/readyup/plugin_api.h); nothing here includes a core header.

#include "readyup/plugin_api.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace skins {

// ---- host (skins_plugin.cpp) --------------------------------------------------------------
extern const ru_api* g_api;
void Log(int level, const char* fmt, ...) RU_PRINTF(2, 3);
bool DebugOn();  // readyup.cfg debug=1 (any thread)
#define SKINS_DEBUG(...)                                             \
  do {                                                               \
    if (::skins::DebugOn()) ::skins::Log(RU_LOG_DEBUG, __VA_ARGS__); \
  } while (0)
// schema_offset("server" scope), -1 if unknown.
int SchemaOffset(const char* cls, const char* field);

// ---- DB rows (tables: see docs/db-contract.md) ------------------------------------------
struct WeaponSkinEntry {
  int weapon_team = 0;  // 2=T, 3=CT, 0=both
  int weapon_defindex = 0;
  int paint_id = 0;
  float wear = 0.0f;
  int seed = 0;
  std::string nametag;
  bool stattrak_enabled = false;
  int stattrak_count = 0;
};
struct WeaponKnifeEntry {
  int weapon_team = 0;
  std::string knife_classname;
};
struct WeaponGloveEntry {
  int weapon_team = 0;
  int glove_defindex = 0;
};
struct WeaponAgentEntry {
  std::string agent_ct;
  std::string agent_t;
};

// ---- loadout cache (loadout.cpp) ---------------------------------------------------------
// All DB access runs on one worker thread owned by the plugin (started in load, joined in
// unload); lookups never block the game thread.
struct TeamLoadout {
  std::unordered_map<int, WeaponSkinEntry> skins_by_defindex;
  std::optional<std::string> knife_classname;
  std::optional<int> glove_defindex;
};
struct Loadout {
  std::unordered_map<int, TeamLoadout> by_team;  // 2=T, 3=CT, 0=both/default
  std::optional<WeaponAgentEntry> agents;
  bool is_admin = false;  // admins get a default knife (resolved on the worker via ru_api is_admin)
};

// Reads <config_dir>/readyup_db.json and starts the worker. false: no DB (skins stay idle).
bool LoadoutStart();
void LoadoutStop();
std::string LoadoutStatus();

void MaybeRefreshAsync(uint64_t steamid64);
void Invalidate(uint64_t steamid64);
bool IsLoaded(uint64_t steamid64);
std::optional<WeaponSkinEntry> FindWeaponSkin(uint64_t steamid64, int weapon_team, int weapon_defindex);
std::optional<std::string> FindKnifeClassname(uint64_t steamid64, int weapon_team);
std::optional<int> FindGloveDefindex(uint64_t steamid64, int weapon_team);
std::optional<WeaponAgentEntry> FindAgents(uint64_t steamid64);
std::optional<int> KnifeClassnameToDefindex(const std::string& classname);
// StatTrak +1 for the row that matched (team 0 rows included), then refetch.
void IncrementStatTrakAsync(uint64_t steamid64, int weapon_team, int weapon_defindex);

// ---- apply (apply.cpp, cosmetics.cpp) ----------------------------------------------------
// Per simulating tick: walks connected players, paints newly seen weapons / knives and applies
// gloves + agents on spawn. Everything the engine does for skins happens here.
void GameFrameTick();
std::string ApplyStatus();
// Dev only: treat bot `slot` as `steamid64` (0 clears). Needs readyup.cfg debug=1.
bool SetDebugAs(int slot, uint64_t steamid64);
bool ApplySpawnCosmetics(void* pawn, uint64_t steamid64, int team, bool late);

// ---- StatTrak (stattrak.cpp) -------------------------------------------------------------
void OnPlayerDeath(const ru_game_event* ev);

}  // namespace skins
