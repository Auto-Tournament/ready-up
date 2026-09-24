#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace readyup {

struct AdminEntry {
  uint64_t steamid64 = 0;
  std::string display_name;
};

struct WeaponSkinEntry {
  int weapon_team = 0;      // 2=T, 3=CT (0 may be treated as both by callers)
  int weapon_defindex = 0;
  int paint_id = 0;
  float wear = 0.0f;
  int seed = 0;
  std::string nametag;
  bool stattrak_enabled = false;
  int stattrak_count = 0;
};

struct WeaponKnifeEntry {
  int weapon_team = 0;  // 2=T, 3=CT (0 may be treated as both by callers)
  std::string knife_classname;
};

struct WeaponGloveEntry {
  int weapon_team = 0;  // 2=T, 3=CT (0 may be treated as both by callers)
  int glove_defindex = 0;
};

struct WeaponAgentEntry {
  std::string agent_ct;
  std::string agent_t;
};

// Postgres-backed global admin store.
//
// Best-effort behavior:
// - If Postgres support is not compiled in, functions return false/empty and set err.
// - If DB config is missing/unreachable, functions return false/empty and set err.
namespace pg {

bool Available();

// Connects if needed and runs `SELECT 1`. Blocking (network I/O): never call on the game thread.
bool Ping(std::string* err);

bool EnsureSchema(std::string* err);
std::vector<AdminEntry> ListAdmins(std::string* err);

// ReadyUp weapon paints/loadouts (best-effort).
// These are intended to be managed externally (e.g. web UI writing rows).
bool EnsureWeaponPaintsSchema(std::string* err);
std::vector<WeaponSkinEntry> ListWeaponSkins(uint64_t steamid64, std::string* err);
std::vector<WeaponKnifeEntry> ListWeaponKnives(uint64_t steamid64, std::string* err);
std::vector<WeaponGloveEntry> ListWeaponGloves(uint64_t steamid64, std::string* err);
std::optional<WeaponAgentEntry> GetWeaponAgents(uint64_t steamid64, std::string* err);

// Updates StatTrak count for an existing weapon skin row.
// Returns false if row doesn't exist or update fails.
bool UpdateWeaponSkinStatTrakCount(uint64_t steamid64,
                                  int weapon_team,
                                  int weapon_defindex,
                                  int stattrak_count,
                                  std::string* err);

// Increments StatTrak count by 1 for an existing weapon skin row.
bool IncrementWeaponSkinStatTrakCount(uint64_t steamid64,
                                      int weapon_team,
                                      int weapon_defindex,
                                      std::string* err);

// Generic key/value settings store (for persisting RU initialization settings).
bool SetSetting(const std::string& key, const std::string& value, std::string* err);
bool ClearSetting(const std::string& key, std::string* err);
std::optional<std::string> GetSetting(const std::string& key, std::string* err);

// Upserts display_name for steamid64.
bool AddAdmin(uint64_t steamid64, const std::string& display_name, std::string* err);
bool RemoveAdmin(uint64_t steamid64, std::string* err);

// Convenience check for auth decisions.
bool IsAdmin(uint64_t steamid64, std::string* err);

}  // namespace pg
}  // namespace readyup

