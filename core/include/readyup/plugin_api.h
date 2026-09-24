/*
 * readyup/plugin_api.h - the stable C ABI between readyup-core and its plugins.
 *
 * readyup-core (the libserver.so shim) owns every piece of engine surface:
 * signatures, offsets, vtable indices, hooks, RTTI checks, game events, the
 * command buffer, chat. Plugins are separate shared objects in
 * csgo/readyup/plugins/<name>.so that only ever talk to the engine through the
 * function table below. A CS2 update therefore needs a core fix only.
 *
 * Rules (see docs/ARCHITECTURE.md for the full contract):
 *   - C only. No C++ types, exceptions, STL containers or ownership crosses
 *     this boundary. Plugins may be written in C++ internally but must catch
 *     everything before returning into the core.
 *   - Every callback runs on the server main thread (the thread that runs
 *     ISource2Server::GameFrame). Every API function except `log` and
 *     `post_to_game_thread` must be called from that thread.
 *   - Strings and structs handed to a callback are only valid for the duration
 *     of that callback. Copy what you need.
 *   - The core removes everything a plugin registered when it unloads it; a
 *     plugin must still join any thread it started before readyup_plugin_unload
 *     returns.
 *
 * Versioning:
 *   READYUP_PLUGIN_API_VERSION = (MAJOR << 16) | MINOR.
 *   - MAJOR changes on any incompatible change. The core refuses plugins built
 *     against a different MAJOR.
 *   - MINOR changes when fields are appended to ru_api or to a callback struct.
 *     Nothing is ever removed or reordered within a MAJOR. The core refuses a
 *     plugin whose MINOR is newer than its own; an older plugin keeps working.
 *   - Structs carry `struct_size`; use RU_API_HAS(api, member) before calling a
 *     member that was added after the MINOR you require.
 */
#ifndef READYUP_PLUGIN_API_H_
#define READYUP_PLUGIN_API_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define READYUP_PLUGIN_API_VERSION_MAJOR 1
#define READYUP_PLUGIN_API_VERSION_MINOR 0
#define READYUP_PLUGIN_API_VERSION \
  ((uint32_t)((READYUP_PLUGIN_API_VERSION_MAJOR << 16) | READYUP_PLUGIN_API_VERSION_MINOR))

#define RU_API_VERSION_MAJOR(v) ((uint32_t)(v) >> 16)
#define RU_API_VERSION_MINOR(v) ((uint32_t)(v)&0xFFFFu)

/* True if `ptr` (a struct with a leading uint32_t struct_size) contains `member`. */
#define RU_API_HAS(ptr, member) \
  ((ptr) != NULL && (ptr)->struct_size >= offsetof(__typeof__(*(ptr)), member) + sizeof((ptr)->member))

#if defined(__GNUC__)
#define READYUP_PLUGIN_EXPORT __attribute__((visibility("default")))
#define RU_PRINTF(fmt_idx, va_idx) __attribute__((format(printf, fmt_idx, va_idx)))
#else
#define READYUP_PLUGIN_EXPORT
#define RU_PRINTF(fmt_idx, va_idx)
#endif

/* Opaque per-plugin handle. Every registration is owned by the handle it was made with. */
typedef struct ru_plugin ru_plugin;

/* Registration handle (0 = failure). Unique for the life of the process. */
typedef uint64_t ru_handle;

typedef enum ru_log_level {
  RU_LOG_INFO = 0,
  RU_LOG_WARN = 1,
  RU_LOG_ERROR = 2,
  RU_LOG_DEBUG = 3 /* only printed when readyup.cfg debug=1 */
} ru_log_level;

/* CS team numbers as used by the engine. */
typedef enum ru_team {
  RU_TEAM_UNASSIGNED = 0,
  RU_TEAM_SPECTATOR = 1,
  RU_TEAM_T = 2,
  RU_TEAM_CT = 3
} ru_team;

/* ---- chat / console commands ------------------------------------------ */

typedef struct ru_command_ctx {
  uint32_t struct_size;
  /* Sender. steamid64 == 0 and is_console == 1 for the server console / RCON. */
  uint64_t steamid64;
  int slot; /* engine player slot, or -1 if unknown (console, or not resolved yet) */
  int is_console;
  const char* name;    /* sender display name ("Console" for the console) */
  const char* text;    /* full trimmed line, e.g. ".hello world" */
  int argc;            /* argv[0] is the command itself (".hello" / "hello_cmd") */
  const char* const* argv;
} ru_command_ctx;

typedef void (*ru_command_fn)(void* user, const ru_command_ctx* ctx);

/* ---- per-tick ---------------------------------------------------------- */

typedef struct ru_tick_info {
  uint32_t struct_size;
  uint64_t frame;  /* simulating frames seen by the core since process start */
  double now;      /* monotonic seconds (CLOCK_MONOTONIC) */
} ru_tick_info;

typedef void (*ru_tick_fn)(void* user, const ru_tick_info* tick);

/* ---- lifecycle events -------------------------------------------------- */

/*
 * Lifecycle events are normalized by the core: they come from engine game
 * events once the core's listener is live and from server log lines until
 * then (`source` tells which). The core picks one source per fact, so a plugin
 * does not get the same round/player change twice (except possibly around the
 * moment the engine listener first comes up at boot).
 */
typedef enum ru_event_type {
  RU_EVENT_ANY = 0, /* subscribe-only: every type */
  RU_EVENT_MAP_START = 1,         /* map */
  RU_EVENT_MATCH_START = 2,       /* CS2 "Match_Start" (warmup end / mp_restartgame) */
  RU_EVENT_ROUND_START = 3,       /* round (1-based if known, else 0) */
  RU_EVENT_ROUND_END = 4,         /* winner (ru_team), reason, team_ct_score/team_t_score if known */
  RU_EVENT_PLAYER_CONNECT = 5,    /* slot, steamid64, name */
  RU_EVENT_PLAYER_DISCONNECT = 6, /* slot, steamid64, name, reason */
  RU_EVENT_PLAYER_TEAM = 7,       /* slot, steamid64, name, team, old_team */
  RU_EVENT_TYPE_COUNT_
} ru_event_type;

typedef enum ru_event_source {
  RU_SOURCE_ENGINE = 0, /* engine game event */
  RU_SOURCE_LOG = 1     /* derived from a server log line */
} ru_event_source;

typedef struct ru_event {
  uint32_t struct_size;
  uint32_t type;   /* ru_event_type */
  uint32_t source; /* ru_event_source */
  int slot;        /* -1 if not applicable / unknown */
  uint64_t steamid64;
  const char* name; /* player name, "" if none */
  const char* map;  /* current map, "" if unknown */
  int team;
  int old_team;
  int winner; /* ru_team, 0 if unknown */
  int reason;
  int round;
  int team_ct_score; /* -1 if unknown */
  int team_t_score;  /* -1 if unknown */
} ru_event;

typedef void (*ru_event_fn)(void* user, const ru_event* ev);

/* Generic deferred call (post_to_game_thread). */
typedef void (*ru_task_fn)(void* user);

/* ---- the function table the core hands to each plugin ------------------ */

enum {
  RU_CHAT_RAW = 1u << 0 /* do not add the ReadyUp chat prefix */
};

typedef struct ru_api {
  uint32_t struct_size;
  uint32_t api_version; /* core's READYUP_PLUGIN_API_VERSION */
  const char* core_version; /* e.g. "0.9.0 (abc1234)" */
  ru_plugin* self;          /* this plugin's handle; pass it to every call */

  /* Logging. Thread-safe. `msg` is one line; a trailing newline is optional. */
  void (*log)(ru_plugin* self, int level, const char* msg);

  /* Queue a server console command (e.g. "mp_restartgame 1"). 1 = queued, 0 = unavailable. */
  int (*server_command)(ru_plugin* self, const char* cmd);

  /* Chat to everyone. flags: RU_CHAT_*. 1 = sent. */
  int (*chat_all)(ru_plugin* self, const char* msg, uint32_t flags);

  /* Chat to one player slot (no prefix is added). 1 = sent, 0 = slot unknown / unavailable. */
  int (*chat_to_slot)(ru_plugin* self, int slot, const char* msg);

  /*
   * Chat command, e.g. ".hello". Must start with '.' or '!' and must not be a
   * command the core or another plugin already owns. Only real players trigger
   * chat commands. Returns 0 on failure (the reason is logged).
   */
  ru_handle (*register_chat_command)(ru_plugin* self, const char* name, ru_command_fn fn, void* user);

  /*
   * Server console / RCON command: a single token, e.g. "hello_status". `ru`
   * and anything the core handles are reserved. Returns 0 on failure.
   */
  ru_handle (*register_console_command)(ru_plugin* self, const char* name, ru_command_fn fn, void* user);

  /* Called once per simulating GameFrame, after the core's own frame work. */
  ru_handle (*on_tick)(ru_plugin* self, ru_tick_fn fn, void* user);

  /* Lifecycle events; type = ru_event_type (RU_EVENT_ANY for all). */
  ru_handle (*subscribe)(ru_plugin* self, uint32_t type, ru_event_fn fn, void* user);

  /* Remove any registration made above. Safe from inside the callback itself. 1 = removed. */
  int (*unregister)(ru_plugin* self, ru_handle handle);

  /*
   * Thread-safe: run fn(user) on the game thread at the next GameFrame. Use it
   * to hand results from your own worker threads back to the game thread. If
   * the plugin is unloaded first, queued tasks are dropped without running.
   */
  int (*post_to_game_thread)(ru_plugin* self, ru_task_fn fn, void* user);

  /* Engine slot of a connected player, or -1. */
  int (*slot_for_steamid)(ru_plugin* self, uint64_t steamid64);

  /* Absolute directory for this plugin's data/config files (csgo/readyup/plugins/<name>/). */
  const char* (*data_dir)(ru_plugin* self);

  /* v1.1+: fields are appended here. Check RU_API_HAS() before use. */
} ru_api;

/* ---- what a plugin exports --------------------------------------------- */

typedef struct ru_plugin_info {
  uint32_t struct_size;  /* sizeof(ru_plugin_info) */
  uint32_t api_version;  /* READYUP_PLUGIN_API_VERSION the plugin was built against */
  const char* name;      /* must equal the file name without ".so", [a-z0-9_-] */
  const char* version;
  const char* author;
  const char* description;
} ru_plugin_info;

/*
 * Exported symbols (extern "C", default visibility):
 *
 *   const ru_plugin_info* readyup_plugin_info(void);
 *       Called right after dlopen, before load. Must not call into the core.
 *
 *   int readyup_plugin_load(const ru_api* api, uint32_t core_api_version);
 *       Game thread. Register commands / ticks / subscriptions here and keep
 *       the api pointer (it stays valid until unload returns). Return 0 on
 *       success; anything else aborts the load (registrations already made are
 *       removed, then the library is closed).
 *
 *   void readyup_plugin_unload(void);
 *       Game thread, at a safe point (no plugin callback on the stack). Stop
 *       and join your threads, free your memory. Registrations are removed by
 *       the core afterwards whether or not you unregister them yourself.
 */
typedef const ru_plugin_info* (*readyup_plugin_info_fn)(void);
typedef int (*readyup_plugin_load_fn)(const ru_api* api, uint32_t core_api_version);
typedef void (*readyup_plugin_unload_fn)(void);

#define READYUP_PLUGIN_INFO_SYMBOL "readyup_plugin_info"
#define READYUP_PLUGIN_LOAD_SYMBOL "readyup_plugin_load"
#define READYUP_PLUGIN_UNLOAD_SYMBOL "readyup_plugin_unload"

/* printf-style convenience wrapper around api->log. */
static inline void ru_logf(const ru_api* api, int level, const char* fmt, ...) RU_PRINTF(3, 4);
static inline void ru_logf(const ru_api* api, int level, const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  api->log(api->self, level, buf);
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* READYUP_PLUGIN_API_H_ */
