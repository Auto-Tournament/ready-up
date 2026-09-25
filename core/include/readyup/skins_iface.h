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
} ru_skins_v1;

#ifdef __cplusplus
}
#endif

#endif /* READYUP_SKINS_IFACE_H */
