/*
 * readyup/plugin_needs_iface.h - plugins the core refused to load on this CS2 build.
 *
 * Provided by the core itself (not by a plugin):
 *
 *   const ru_plugin_needs_iface_v1* n =
 *       api->get_interface(api->self, RU_PLUGIN_NEEDS_IFACE_NAME, RU_PLUGIN_NEEDS_IFACE_VERSION);
 *
 * A plugin whose csgo/readyup/plugins/<name>.needs.json names an engine-surface entry that did
 * not resolve, or a required schema field that is missing, is not loaded (docs/CS2-COMPAT.md,
 * "Plugin needs"). plugins/fleet reports the list in hello.plugins_disabled.
 */
#ifndef READYUP_PLUGIN_NEEDS_IFACE_H_
#define READYUP_PLUGIN_NEEDS_IFACE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_PLUGIN_NEEDS_IFACE_NAME "readyup.core.plugin_needs"
#define RU_PLUGIN_NEEDS_IFACE_VERSION 1u

typedef struct ru_plugin_needs_iface_v1 {
  uint32_t struct_size; /* sizeof(ru_plugin_needs_iface_v1) */
  /* JSON array [{"name":"skins","reason":"missing X after CS2 build N"}], "[]" when none.
   * Writes at most cap-1 bytes plus a NUL; returns the full length (call again with a bigger
   * buffer if it is >= cap). Any thread. */
  uint32_t (*disabled_json)(char* buf, uint32_t cap);
} ru_plugin_needs_iface_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* READYUP_PLUGIN_NEEDS_IFACE_H_ */
