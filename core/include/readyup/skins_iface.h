#ifndef READYUP_SKINS_IFACE_H
#define READYUP_SKINS_IFACE_H
/*
 * readyup.skins.v1: the skins plugin (plugins/skins, skins.so) to other plugins. The midas
 * plugin paints its gold weapons through it instead of writing econ attributes itself. Published
 * with
 *
 *   api->provide_interface(api->self, RU_SKINS_IFACE_NAME, RU_SKINS_IFACE_VERSION, &iface);
 *
 * Only there when skins.so is loaded (Full bundle, added on purpose). Same ABI rules as
 * plugin_api.h: plain C, members only appended, struct_size first. Game thread.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_SKINS_IFACE_NAME "readyup.skins.v1"
#define RU_SKINS_IFACE_VERSION 1u

typedef struct ru_skins_v1 {
  uint32_t struct_size; /* sizeof(ru_skins_v1) of the provider */
  /* 1 = skins.so paints weapons right now; 0 while it is inert (ruleset valve, the cosmetics
   * override "inventory", READYUP_DISABLE_SKINS) or its schema fields are missing. */
  int (*active)(void);
  /* Paints the weapon entity `weapon_handle` (an entity handle, e.g. from m_hMyWeapons) with
   * `paint_kit` (items_game paint kit id; wear 0..1 and seed as in loadouts.json), shown as an
   * item of `steamid64`. Legacy paint kits get the legacy model. From then on skins.so leaves
   * that weapon alone (no loadout over it, `.skins reload` included) until paint_weapon is called
   * again with paint_kit 0, which hands it back: stock paint now, the holder's own loadout (if
   * any) on the next tick. Returns 1 when written; 0 when inactive or the handle is stale. */
  int (*paint_weapon)(uint32_t weapon_handle, uint64_t steamid64, int32_t paint_kit, float wear, int32_t seed);
  /* Per-player paint: while set, every new weapon skins.so paints for `steamid64` (spawned or
   * bought; not one another player owned first) gets `paint_kit` / wear / seed when it is created,
   * before it is networked, instead of the loadout skin (knives keep the loadout knife model). A weapon painted
   * after it was networked keeps its old wear on clients, which is why this exists. paint_kit 0
   * clears it; weapons already painted keep their look. Not kept across a skins.so reload: set it
   * again (midas re-sends every 5 s). Returns 1 when stored. Appended; check RU_API_HAS. */
  int (*set_player_paint)(uint64_t steamid64, int32_t paint_kit, float wear, int32_t seed);
  /* Swaps the weapons the live player in `slot` holds for new ones (removed now, given two ticks
   * later with their ammo), so they are painted when created: after set_player_paint, or when a
   * loadout changed. Grenades, C4 and weapons another player owned first stay. 1 = queued, 0 =
   * nothing to do or unavailable (core without player_give_item). Appended; check RU_API_HAS. */
  int (*refresh_weapons)(int slot);
} ru_skins_v1;

#ifdef __cplusplus
}
#endif

#endif /* READYUP_SKINS_IFACE_H */
