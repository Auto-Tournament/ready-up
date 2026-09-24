#pragma once

#include "readyup/postgres.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace readyup::weapon_paints {

// In-memory cache of web-managed weapon paint/loadout data (tables: see docs/skins-db-contract.md).
//
// Best-effort:
// - If Postgres is unavailable or not configured, the cache simply won't load.
// - All DB access happens on background threads; lookups never block the game thread.

struct TeamLoadout {
  std::unordered_map<int, readyup::WeaponSkinEntry> skins_by_defindex;

  // Optional cosmetics.
  std::optional<std::string> knife_classname;
  std::optional<int> glove_defindex;
};

struct Loadout {
  // Keys are CS team numbers (2=T, 3=CT, 0=both/default).
  std::unordered_map<int, TeamLoadout> by_team;

  // Optional agent models.
  std::optional<readyup::WeaponAgentEntry> agents;

  // ReadyUp admin (resolved off-thread with the loadout); admins get a default knife.
  bool is_admin = false;
};

// Starts background schema ensure (best-effort).
void EnsureSchemaAsync();

// If loadout is missing/stale, starts a background refresh (best-effort).
void MaybeRefreshAsync(uint64_t steamid64);

// Clears any cached entry immediately.
void Invalidate(uint64_t steamid64);

// True once a loadout (possibly empty) has been loaded for this player at least once.
bool IsLoaded(uint64_t steamid64);

// Lookup helpers (return empty if cache hasn't loaded yet). Never block.
std::optional<readyup::WeaponSkinEntry> FindWeaponSkin(uint64_t steamid64, int weapon_team, int weapon_defindex);
std::optional<std::string> FindKnifeClassname(uint64_t steamid64, int weapon_team);
std::optional<int> FindGloveDefindex(uint64_t steamid64, int weapon_team);
std::optional<readyup::WeaponAgentEntry> FindAgents(uint64_t steamid64);

// Knife classname ("weapon_knife_karambit") -> item defindex (507). nullopt if unknown/default.
std::optional<int> KnifeClassnameToDefindex(const std::string& classname);

// Called from the server GameFrame hook (after the original GameFrame, before snapshots are sent).
// Walks connected players, applies weapon skins / knives to newly seen weapons and gloves / agents
// on spawn. All engine calls made by the skins feature happen here, on the game thread.
void GameFrameTick();

// Internal (weapon_paints_cosmetics.cpp): gloves + agent model for a freshly spawned pawn.
// Returns false if the player's loadout isn't loaded yet (caller retries).
bool ApplySpawnCosmetics(void* pawn, uint64_t steamid64, int team, bool late);

}  // namespace readyup::weapon_paints
