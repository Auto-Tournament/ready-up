#ifndef READYUP_PRACTICE_IFACE_H
#define READYUP_PRACTICE_IFACE_H
/*
 * readyup.practice.v1: the practice plugin (plugins/practice, practice.so) to other plugins. The
 * match plugin hands `.ru mode practice` to it and shows its help line in `.help` while practice
 * is on. Published with
 *
 *   api->provide_interface(api->self, RU_PRACTICE_IFACE_NAME, RU_PRACTICE_IFACE_VERSION, &iface);
 *
 * Same ABI rules as plugin_api.h: plain C, members only appended, struct_size first. Game thread.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_PRACTICE_IFACE_NAME "readyup.practice.v1"
#define RU_PRACTICE_IFACE_VERSION 1u

typedef struct ru_practice_v1 {
  uint32_t struct_size; /* sizeof(ru_practice_v1) of the provider */
  /* 1 = practice mode is on. */
  int (*active)(void);
  /* on = 1 / 0: enter / leave practice mode (cvar cfg, everyone respawned). Returns 1 on success;
   * 0 when refused (a match is loaded), and *why (if not NULL) points to a static reason. */
  int (*set_active)(int on, const char** why);
  /* One chat line with the practice commands. Static string. */
  const char* (*help)(void);
} ru_practice_v1;

#ifdef __cplusplus
}
#endif

#endif /* READYUP_PRACTICE_IFACE_H */
