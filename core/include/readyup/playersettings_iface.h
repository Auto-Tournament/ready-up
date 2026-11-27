/*
 * readyup.playersettings.v1: what other plugins tell the playersettings plugin (the config read for
 * the platform's profile Config tab).
 *
 * The fleet plugin calls set_player_off with a player's `read_config` from match.assign (and an
 * add_player op): off = the player turned their config off on the platform, so their game is not
 * asked. Look it up with get_interface(RU_PLAYERSETTINGS_IFACE_NAME, 1); check struct_size before
 * using a field. Game thread.
 */
#ifndef READYUP_PLAYERSETTINGS_IFACE_H
#define READYUP_PLAYERSETTINGS_IFACE_H

#include <stdint.h>

#define RU_PLAYERSETTINGS_IFACE_NAME "readyup.playersettings.v1"
#define RU_PLAYERSETTINGS_IFACE_VERSION 1

typedef struct ru_playersettings_v1 {
  uint32_t struct_size;
  /* 1 = do not read this player's config (and drop a read under way); 0 = read it again. */
  void (*set_player_off)(uint64_t steamid64, int off);
} ru_playersettings_v1;

#endif /* READYUP_PLAYERSETTINGS_IFACE_H */
