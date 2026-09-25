#ifndef READYUP_MATCH_IFACE_H
#define READYUP_MATCH_IFACE_H
/*
 * readyup.match.v1: what the match plugin (plugins/match, match.so) tells the core's local
 * status endpoint (docs/FLEET.md §17: /status, /stream, /metrics). The plugin publishes it with
 *
 *   api->provide_interface(api->self, RU_MATCH_IFACE_NAME, RU_MATCH_IFACE_VERSION, &iface);
 *
 * and the core looks it up on the game thread whenever it rebuilds the status snapshot (a few
 * times a second). Without it (no match.so, or it is being reloaded) /status reports
 * `summary.match_plugin: "none"`, mode/phase `idle` and `update_safe: true`.
 *
 * Same ABI rules as plugin_api.h: plain C, members are only ever appended, every struct starts
 * with struct_size and readers check it before touching trailing members. Game thread only;
 * must not block.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_MATCH_IFACE_NAME "readyup.match.v1"
#define RU_MATCH_IFACE_VERSION 1u

typedef struct ru_match_status {
  uint32_t struct_size; /* set by the CALLER to sizeof(ru_match_status); fill only what fits */
  /* 1 = nothing would be lost by updating / restarting the server now: no real match between
   * loading and series end, no demo upload pending. Scrims, practice and idle are safe. The core
   * also asks the fleet plugin (readyup.fleet.v1 update_blocked). */
  int32_t update_safe;
  /* JSON object: the flat /status `summary` fields the plugin owns (mode, ru_mode, phase, map,
   * map_number, num_maps, round, score, series_score, players, ready, match_id, slug, teams,
   * paused, knife, countdown_s, demo_uploads_pending). Owned by the plugin; valid until its next
   * get_status call or until it unloads. Never NULL. */
  const char* summary_json;
  /* JSON object: MatchState (docs/FLEET.md §9.1) while a match or scrim is loaded, else NULL.
   * The core adds `server_id` (from readyup.fleet.v1). Same lifetime as summary_json. */
  const char* state_json;
  /* The ru mode string ("idle", "match_warmup", ...). Same lifetime. Never NULL. */
  const char* ru_mode;
} ru_match_status;

typedef struct ru_match_v1 {
  uint32_t struct_size; /* sizeof(ru_match_v1) of the provider */
  /* Game thread. Fills *out (up to out->struct_size bytes). Returns 1 on success. */
  int (*get_status)(ru_match_status* out);
  /* v1.1 (rulesets, docs/ESPORTS-MODE.md). Game thread. 1 = players' inventories must not be
   * modified right now (ruleset "valve", or the cosmetics override "inventory"): the skins plugin
   * stays inert. 0 = allowed. */
  int (*inventory_locked)(void);
  /* Effective ruleset of the loaded match (readyup.cfg's when none is loaded): "default" or
   * "valve". Static string. */
  const char* (*ruleset)(void);
  /* v1.2 (practice plugin, plugins/practice). Game thread. on = 1: the match flow enters its
   * practice mode (no scrim warmup, heartbeat "warmup", ru_mode "practice"); refused (0) while a
   * match is loaded. on = 0: back to idle. Returns 1 when the mode is what was asked. The
   * practice plugin execs the cvar cfgs and respawns players itself. */
  int (*set_practice)(int on);
  /* v1.x: members are appended here. */
} ru_match_v1;

#ifdef __cplusplus
}
#endif

#endif /* READYUP_MATCH_IFACE_H */
