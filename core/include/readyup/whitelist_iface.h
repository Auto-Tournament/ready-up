#ifndef READYUP_WHITELIST_IFACE_H
#define READYUP_WHITELIST_IFACE_H
/*
 * readyup.whitelist.v1: the whitelist plugin (plugins/whitelist, whitelist.so) to other plugins.
 * The fleet link's `whitelist.set` command replaces the whole list through it. Published with
 *
 *   api->provide_interface(api->self, RU_WHITELIST_IFACE_NAME, RU_WHITELIST_IFACE_VERSION, &iface);
 *
 * Same ABI rules as plugin_api.h: plain C, members only appended, struct_size first. Game thread.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_WHITELIST_IFACE_NAME "readyup.whitelist.v1"
#define RU_WHITELIST_IFACE_VERSION 1u

typedef struct ru_whitelist_v1 {
  uint32_t struct_size; /* sizeof(ru_whitelist_v1) of the provider */
  /* Replaces the state (on/off + the whole list) and saves it. 1 = saved. */
  int (*set)(int enabled, const uint64_t* steamids, uint32_t count);
  /* 1 = on. *count (if not NULL) = players on the list. */
  int (*get)(uint32_t* count);
} ru_whitelist_v1;

#ifdef __cplusplus
}
#endif

#endif /* READYUP_WHITELIST_IFACE_H */
