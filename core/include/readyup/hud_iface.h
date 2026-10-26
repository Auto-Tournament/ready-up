#ifndef READYUP_HUD_IFACE_H
#define READYUP_HUD_IFACE_H
/*
 * readyup.hud.v1: the hud plugin (plugins/hud, hud.so) to other plugins. Draws Panorama layouts
 * from the Auto Tournament HUD workshop addon (repo hud/) through CS2's custom_hud_layout entity:
 * the server sets dialog-variable text and CSS classes on the layout's panels, per player or for
 * everyone. Published with
 *
 *   api->provide_interface(api->self, RU_HUD_IFACE_NAME, RU_HUD_IFACE_VERSION, &iface);
 *
 * The plugin keeps the desired state and pushes it again when it has to recreate the entity (map
 * change, a stray removed) and shortly after a player takes a slot, so callers just say what each
 * player should see. A player without the addon sees nothing: keep the chat / center-HTML version
 * as the fallback.
 *
 * Same ABI rules as plugin_api.h: plain C, members only appended, struct_size first. Game thread.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_HUD_IFACE_NAME "readyup.hud.v1"
#define RU_HUD_IFACE_VERSION 1u

/* Slot value meaning "everyone" (the layout's global state). */
#define RU_HUD_ALL (-1)

typedef struct ru_hud_v1 {
  uint32_t struct_size; /* sizeof(ru_hud_v1) of the provider */
  /* 1 when this server can draw custom HUDs (schema, entity create, allocator and the dialog
   * variable vtable all verified on this CS2 build). 0: callers use their fallback. */
  int (*available)(void);
  /* The layout by its SOURCE path, e.g. "panorama/layout/custom_game/at_kit.xml". Creates the
   * entity on first use (and again after a map change). Returns a handle > 0, 0 on failure. Several
   * callers asking for the same layout share one entity. */
  int (*open)(const char* layout);
  /* Dialog variable `var` ("text") on panel `panel_id` for `slot` (RU_HUD_ALL = everyone). 1 = done. */
  int (*set_text)(int hud, int slot, const char* panel_id, const char* var, const char* value);
  /* CSS class `cls` on panel `panel_id` for `slot` (RU_HUD_ALL = everyone), on or off. 1 = done. */
  int (*set_class)(int hud, int slot, const char* panel_id, const char* cls, int on);
  /* Forget everything set for `slot` (call on disconnect: slots are reused). 1 = done. */
  int (*clear_slot)(int hud, int slot);
  /* v1.x: members are appended here. */
} ru_hud_v1;

#ifdef __cplusplus
}
#endif

#endif /* READYUP_HUD_IFACE_H */
