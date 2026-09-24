#ifndef READYUP_FLEET_IFACE_H
#define READYUP_FLEET_IFACE_H
/*
 * readyup.fleet.v1: the part of the fleet plugin's interface (plugins/fleet, docs/FLEET.md)
 * that the core reads for the local status endpoint (docs/FLEET.md §17, `platform` in
 * /status). The fleet plugin publishes it with
 *
 *   api->provide_interface(api->self, RU_FLEET_IFACE_NAME, RU_FLEET_IFACE_VERSION, &iface);
 *
 * and the core looks it up on the game thread whenever it rebuilds the status snapshot (a few
 * times a second). Without it the core reports `platform: {mode: "standalone"}`.
 *
 * Same ABI rules as plugin_api.h: plain C, members are only ever appended, every struct
 * starts with struct_size and readers check it before touching trailing members.
 * Everything here is called on the game thread only and must not block.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_FLEET_IFACE_NAME "readyup.fleet.v1"
#define RU_FLEET_IFACE_VERSION 1u

typedef enum ru_fleet_state {
  RU_FLEET_STATE_OFFLINE = 0,   /* configured, not connected (reconnecting with backoff) */
  RU_FLEET_STATE_ONLINE = 1,    /* WebSocket up, hello accepted */
  RU_FLEET_STATE_ENROLLING = 2, /* enrollment code present, no credentials yet */
  RU_FLEET_STATE_REJECTED = 3   /* credentials revoked / fenced; needs an operator */
} ru_fleet_state;

typedef struct ru_fleet_status {
  uint32_t struct_size;    /* set by the CALLER to sizeof(ru_fleet_status); fill only what fits */
  uint32_t state;          /* ru_fleet_state */
  uint64_t since_ms;       /* unix ms of the last state change (/status reports seconds) */
  uint32_t reconnects;     /* reconnect attempts since the process started */
  uint32_t spool_msgs;     /* messages waiting in the offline spool */
  int32_t auto_pause_in_s; /* seconds until the offline auto-pause (D12); -1 = no countdown */
  int32_t update_blocked;  /* 1 = the platform still needs this server (e.g. demo not stored) */
  char server_id[64];      /* NUL-terminated; "" before enrollment */
} ru_fleet_status;

typedef struct ru_fleet_v1 {
  uint32_t struct_size; /* sizeof(ru_fleet_v1) of the provider */
  /* Game thread. Fills *out (up to out->struct_size bytes). Returns 1 on success. */
  int (*get_status)(ru_fleet_status* out);
  /* v1.x: members are appended here. */
} ru_fleet_v1;

#ifdef __cplusplus
}
#endif

#endif /* READYUP_FLEET_IFACE_H */
