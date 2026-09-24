// StatTrak: +1 kill on the attacker's configured skin row when that row has StatTrak enabled.
// Driven by the raw engine `player_death` event (ru_api subscribe_game_event).
#include "skins.h"

#include <optional>
#include <string>

namespace skins {
namespace {

std::optional<int> DefindexForDeathWeaponString(const char* weapon) {
  // `player_death` typically provides `weapon` as a short token like "ak47" (no "weapon_" prefix).
  if (!weapon || !*weapon) return std::nullopt;
  std::string w(weapon);
  if (w.rfind("weapon_", 0) == 0) w = w.substr(7);

  // Map based on cs2-WeaponPaints `Variables.cs` WeaponDefindex entries (subset).
  // Expand as needed; unknown weapons return nullopt.
  // Pistols
  if (w == "deagle") return 1;
  if (w == "elite") return 2;
  if (w == "fiveseven") return 3;
  if (w == "glock") return 4;
  if (w == "tec9") return 30;
  if (w == "hkp2000") return 32;
  if (w == "p250") return 36;
  if (w == "usp_silencer") return 61;
  if (w == "cz75a") return 63;
  if (w == "revolver") return 64;

  // Rifles/SMGs/Heavies
  if (w == "ak47") return 7;
  if (w == "aug") return 8;
  if (w == "awp") return 9;
  if (w == "famas") return 10;
  if (w == "g3sg1") return 11;
  if (w == "galilar") return 13;
  if (w == "m249") return 14;
  if (w == "m4a1") return 16;
  if (w == "mac10") return 17;
  if (w == "p90") return 19;
  if (w == "mp5sd") return 23;
  if (w == "ump45") return 24;
  if (w == "xm1014") return 25;
  if (w == "bizon") return 26;
  if (w == "mag7") return 27;
  if (w == "negev") return 28;
  if (w == "sawedoff") return 29;
  if (w == "taser") return 31;
  if (w == "mp7") return 33;
  if (w == "mp9") return 34;
  if (w == "nova") return 35;
  if (w == "scar20") return 38;
  if (w == "sg556") return 39;
  if (w == "ssg08") return 40;
  if (w == "m4a1_silencer") return 60;

  // Knives (often show as "knife", "knife_t", etc. on death events).
  if (w == "knife" || w == "knife_t" || w == "bayonet") return 500;  // cosmetic defindex is per-player loadout

  return std::nullopt;
}

}  // namespace

void OnPlayerDeath(const ru_game_event* ev) {
  const int attackerSlot = g_api->ev_get_player_slot(g_api->self, ev, "attacker");
  if (attackerSlot < 0) return;
  ru_player p{};
  p.struct_size = sizeof(p);
  if (g_api->get_player(g_api->self, attackerSlot, &p) != 1 || p.steamid64 == 0) return;
  const auto def = DefindexForDeathWeaponString(g_api->ev_get_string(g_api->self, ev, "weapon", ""));
  const int team = g_api->ev_get_int(g_api->self, ev, "attackerteam", 0);  // 2=T, 3=CT
  if (!def || team == 0) return;
  // Only when the configured row has StatTrak on; increment the row that matched (may be team 0).
  const auto skin = FindWeaponSkin(p.steamid64, team, *def);
  if (skin && skin->stattrak_enabled) IncrementStatTrakAsync(p.steamid64, skin->weapon_team, *def);
}

}  // namespace skins
