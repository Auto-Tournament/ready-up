#ifndef READYUP_MAPVOTE_IFACE_H
#define READYUP_MAPVOTE_IFACE_H
/*
 * readyup.mapvote.v1: the map vote plugin (plugins/mapvote, mapvote.so) to other plugins. The
 * fleet link's `mapvote.set` command gives a server its map pool through it (a saved setup on
 * the platform). Published with
 *
 *   api->provide_interface(api->self, RU_MAPVOTE_IFACE_NAME, RU_MAPVOTE_IFACE_VERSION, &iface);
 *
 * Same ABI rules as plugin_api.h: plain C, members only appended, struct_size first. Game thread.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_MAPVOTE_IFACE_NAME "readyup.mapvote.v1"
#define RU_MAPVOTE_IFACE_VERSION 1u

typedef struct ru_mapvote_v1 {
  uint32_t struct_size; /* sizeof(ru_mapvote_v1) of the provider */
  /* Replaces the map pool and saves it (<data_dir>/pool.txt), so it wins over mapvote.cfg's maps=
   * from now on and after restarts. Entries: map names or "workshop/<id>[/<name>]". count 0 clears
   * the saved pool (mapvote.cfg again). 1 = saved and in use. */
  int (*set_pool)(const char* const* maps, uint32_t count);
} ru_mapvote_v1;

#ifdef __cplusplus
}
#endif

#endif
