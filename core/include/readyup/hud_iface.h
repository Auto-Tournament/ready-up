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

/* A button click on a `screen`: the button's command, e.g. ".r". Game thread. */
typedef void (*ru_hud_click_fn)(void* user, int slot, const char* command);
/* `.menu` providers (register_menu): does this plugin's menu fit `slot` now; open it. Game thread. */
typedef int (*ru_hud_menu_active_fn)(void* user, int slot);
typedef void (*ru_hud_menu_open_fn)(void* user, int slot, uint64_t steamid64);

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
  /* v1.1 (check struct_size): the clickable HUD (at_ui.xml). Shows `spec` to `slot` (0-63): a
   * whole screen as `key=value` lines, listed in plugins/hud/hud_screen.h (id, where, kind, title,
   * buttons `btn=label|tone|state|command`, mouse off|b|on, ...). Call it as often as you like: the
   * same spec costs nothing, the same id updates in place, a new id plays the leave / enter
   * animation, "" hides. A click on a button calls `on_click(user, slot, command)` on the game
   * thread with that button's command (typically the chat command it stands for, e.g. ".r"). The
   * plugin owns the cursor: it captures it for mouse=on (or while the buy menu is open for
   * mouse=b), never with chips=1, and gives it back on a click, a new id, "" or a disconnect.
   * 1 = done. */
  int (*screen)(int slot, const char* spec, ru_hud_click_fn on_click, void* user);
  /* v1.2 (check struct_size): menus from any plugin. A menu is drawn over what `screen` shows for
   * `slot` while it is up: the latest menu from any owner wins, the base screen waits and comes
   * back when the last menu closes. `owner` is a short plugin name ("practice"); spec "" closes
   * that owner's menu; slot RU_HUD_ALL with "" closes it for everyone (call it on unload). Same
   * spec format and cursor rules as `screen`. 1 = done. */
  int (*menu)(int slot, const char* owner, const char* spec, ru_hud_click_fn on_click, void* user);
  /* v1.2: what `.menu` opens. `.menu` (registered by hud.so) asks every provider, highest
   * `priority` first, whether its menu fits this player now (`active`) and opens the first that
   * does (`open`). Priorities in use: match 0, mapvote 10, movement 20, practice 30. Registering an
   * owner again replaces it; active or open NULL unregisters it and closes its menus (call that on
   * unload). 1 = done. */
  int (*register_menu)(const char* owner, int priority, ru_hud_menu_active_fn active, ru_hud_menu_open_fn open, void* user);
  /* v1.3 (check struct_size): the cursor for `slot` on a layout from `open` (each layout holds its
   * own), on (1) or off (0): hover (CSS :hover, tooltips) needs it. Give it back when done and on
   * disconnect. `screen` and `menu` manage at_ui's cursor themselves. 1 = done. */
  int (*set_capture)(int hud, int slot, int on);
  /* v1.x: members are appended here. */
} ru_hud_v1;

#ifdef __cplusplus
}
#endif

#endif /* READYUP_HUD_IFACE_H */
