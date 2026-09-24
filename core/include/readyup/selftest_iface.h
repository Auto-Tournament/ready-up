/*
 * readyup/selftest_iface.h - how a plugin adds its own lines to `ru selftest`.
 *
 * Not part of ru_api (no API version bump): a plugin publishes this struct with
 *
 *   api->provide_interface(api->self, "readyup.selftest.<plugin name>", 1, &iface);
 *
 * and `ru selftest` calls `run` for every such interface in its [plugins] section.
 *
 * Rules for `run`:
 *   - It may be called from ANY thread (console, RCON, the selftest-and-quit watchdog),
 *     concurrently with the plugin's own threads, and even while the plugin is unloading
 *     (the core guarantees the image stays mapped until `run` returns). Read your own
 *     state under your own lock; never touch memory your unload frees without that lock.
 *   - It must be fast and must not block (no network, no disk).
 *   - It must not call any ru_api member except `log`.
 *   - `add(ctx, status, name, detail)` once per check. status is one of
 *     "OK" | "FAIL" | "PEND" | "SKIP" | "INFO" | "WARN" (OK and FAIL count towards PASS n/n,
 *     FAIL fails the selftest, PEND is shown as pending). Strings are copied.
 */
#ifndef READYUP_SELFTEST_IFACE_H_
#define READYUP_SELFTEST_IFACE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_SELFTEST_IFACE_PREFIX "readyup.selftest."
#define RU_SELFTEST_IFACE_VERSION 1u

typedef void (*ru_selftest_add_fn)(void* ctx, const char* status, const char* name, const char* detail);

typedef struct ru_selftest_iface_v1 {
  uint32_t struct_size; /* sizeof(ru_selftest_iface_v1) */
  void (*run)(ru_selftest_add_fn add, void* ctx);
} ru_selftest_iface_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* READYUP_SELFTEST_IFACE_H_ */
