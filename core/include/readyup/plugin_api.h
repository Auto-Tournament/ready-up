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
#define READYUP_PLUGIN_API_VERSION_MINOR 3
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
  /* v1.2: 1 on simulating frames. on_tick callbacks only ever see 1; on_frame callbacks also
   * run on non-simulating frames (map load, hibernation) and see 0 there. `frame` only counts
   * simulating frames. */
  int simulating;
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

/* ---- v1.1: players ------------------------------------------------------ */

/*
 * One connected player, from the core's identity/team registry. The caller owns
 * the struct and sets struct_size = sizeof(ru_player) before passing it in; the
 * core fills only the fields that fit. `name` is copied (NUL-terminated).
 */
typedef struct ru_player {
  uint32_t struct_size;
  int slot;           /* engine player slot, -1 if not seen in an engine event yet (bots: always -1) */
  uint64_t steamid64; /* 0 for bots */
  int team;           /* ru_team */
  int is_bot;
  int connected;
  char name[128];
  /* v1.2: the player's `<N>` in server log lines ("Name<N><[U:1:x]><CT>"; CS2 logs the player
   * slot there, and it is what `kickid` takes). -1 if not seen in a log line yet. Bots have one too. */
  int userid;
} ru_player;

/* for_each_player callback. Return non-zero to continue, 0 to stop. */
typedef int (*ru_player_fn)(void* user, const ru_player* player);

/* ---- v1.1: raw engine game events --------------------------------------- */

/*
 * An engine game event (e.g. "player_death"). Opaque: read it with the ev_get_*
 * members of ru_api. Valid only for the duration of the callback; raw events are
 * delivered synchronously on the game thread while the engine dispatches them.
 */
typedef struct ru_game_event ru_game_event;
typedef void (*ru_game_event_fn)(void* user, const char* name, const ru_game_event* ev);

/* ---- v1.1: server log lines ---------------------------------------------- */

/* One server log line (the core's own output is filtered out). Queued; `line` is a copy
 * that lives for the duration of the callback. */
typedef void (*ru_log_line_fn)(void* user, const char* line);

/* ---- v1.1: entities -------------------------------------------------------- */

typedef enum ru_bodygroup_result {
  RU_BODYGROUP_OK = 0,
  RU_BODYGROUP_UNAVAILABLE = 1, /* an engine function did not resolve (or its gamedata is not installed) */
  RU_BODYGROUP_NO_MODEL = 2,    /* the entity has no loaded model yet (e.g. right after set_model): retry later */
  RU_BODYGROUP_NO_GROUP = 3     /* the model has no bodygroup with that name */
} ru_bodygroup_result;

/* entity_system_status */
enum {
  RU_ENTSYS_PENDING = 0, /* no map loaded yet / schema not ready */
  RU_ENTSYS_OK = 1,
  RU_ENTSYS_FAILED = 2
};

/* ---- v1.1: admins ---------------------------------------------------------- */

/*
 * Admin provider (set_admin_provider). Called on whatever thread called is_admin, so it
 * must be thread-safe and may block (e.g. a DB lookup). Return 1 = admin, 0 = not an admin,
 * -1 = no opinion (the core falls back to its built-in check).
 */
typedef int (*ru_admin_provider_fn)(void* user, uint64_t steamid64);

/* ---- the function table the core hands to each plugin ------------------ */

enum {
  RU_CHAT_RAW = 1u << 0 /* do not add the Ready Up chat prefix */
};

/* ---- v1.2: command flags (register_chat_command_ex / register_console_command_ex) ---- */
enum {
  /*
   * Chat: the sender's chat line is not shown to anyone. Best effort: the core swallows it in
   * its ClientCommand hook, before the engine prints it; if that hook is not installed on this
   * build the line stays visible (the command still runs, via the server log).
   */
  RU_CMD_HIDE = 1u << 0,
  /*
   * Console: observe only. The engine still executes the line and the command is not
   * consumed (e.g. watch `tv_delay 5`). Several plugins may observe the same name; an observer
   * never conflicts with a normal registration.
   */
  RU_CMD_OBSERVE = 1u << 1
};


typedef struct ru_api {
  uint32_t struct_size;
  uint32_t api_version; /* core's READYUP_PLUGIN_API_VERSION */
  const char* core_version; /* e.g. "0.9.0 (abc1234)" */
  ru_plugin* self;          /* this plugin's handle; pass it to every call */

  /* Logging. Thread-safe. `msg` is one line (any length); a trailing newline is optional. */
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

  /* ==== v1.1 ============================================================
   * Everything below was appended in 1.1. A plugin that requires 1.1 in its
   * ru_plugin_info.api_version can call these directly (older cores refuse to
   * load it); a plugin that requires 1.0 must check RU_API_HAS(api, member).
   * Game thread only unless marked "any thread".
   */

  /* -- output -- */

  /* Center-screen HTML panel to ONE client (never broadcast). 1 = sent. */
  int (*center_html_to_slot)(ru_plugin* self, int slot, const char* html, int seconds);
  /* The same panel to every connected human, one client at a time. Returns how many got it. */
  int (*center_html_all)(ru_plugin* self, const char* html, int seconds);

  /* -- players -- */

  /* Connected player in `slot`. 1 = found (out filled), 0 = none. */
  int (*get_player)(ru_plugin* self, int slot, ru_player* out);
  /* Connected human with this SteamID64. 1 = found. */
  int (*get_player_by_steamid)(ru_plugin* self, uint64_t steamid64, ru_player* out);
  /* Calls fn for every connected human, then every known bot. Returns how many were visited. */
  int (*for_each_player)(ru_plugin* self, ru_player_fn fn, void* user);

  /* -- raw engine events -- */

  /*
   * Engine game event by name ("player_death", "item_pickup", ...). Delivered synchronously
   * on the game thread while the engine dispatches it, so keep the callback short and never
   * keep `ev`. The core registers its listener for the name (best-effort: an event the
   * engine does not know is never delivered). Returns 0 on failure.
   */
  ru_handle (*subscribe_game_event)(ru_plugin* self, const char* name, ru_game_event_fn fn, void* user);
  int (*ev_get_int)(ru_plugin* self, const ru_game_event* ev, const char* key, int def);
  double (*ev_get_float)(ru_plugin* self, const ru_game_event* ev, const char* key, double def);
  uint64_t (*ev_get_uint64)(ru_plugin* self, const ru_game_event* ev, const char* key, uint64_t def);
  /* Valid only during the callback. Never NULL (returns `def`, or "" if def is NULL). */
  const char* (*ev_get_string)(ru_plugin* self, const ru_game_event* ev, const char* key, const char* def);
  /* Player slot stored in a player key ("userid", "attacker", ...), or -1. */
  int (*ev_get_player_slot)(ru_plugin* self, const ru_game_event* ev, const char* key);
  /* Controller / pawn entity of a player key, or NULL. Valid for this frame only. */
  void* (*ev_get_player_controller)(ru_plugin* self, const ru_game_event* ev, const char* key);
  void* (*ev_get_player_pawn)(ru_plugin* self, const ru_game_event* ev, const char* key);

  /* -- log lines -- */

  /* Every server log line (in-process logging listener, or the file tail fallback). Queued. */
  ru_handle (*subscribe_log_line)(ru_plugin* self, ru_log_line_fn fn, void* user);

  /* -- schema and entities -- */

  /*
   * Byte offset of a networked field, looked up by name in the server's schema (base
   * classes included), e.g. schema_offset(self, "CCSPlayerPawn", "m_EconGloves").
   * -1 if unknown or the schema system is not verified. Never hard-code offsets.
   */
  int (*schema_offset)(ru_plugin* self, const char* class_name, const char* field_name);
  /* RU_ENTSYS_*: until OK every entity_* lookup below returns NULL. */
  int (*entity_system_status)(ru_plugin* self);
  /* Entity by index (0 = world, 1..64 = player controllers), or NULL. Valid for this frame only. */
  void* (*entity_by_index)(ru_plugin* self, int index);
  /* Entity from a CHandle (as stored in m_hPlayerPawn, m_hMyWeapons, ...), or NULL. */
  void* (*entity_from_handle)(ru_plugin* self, uint32_t handle);
  /* CHandle of an entity, 0xFFFFFFFF if unknown. Keep handles across frames, never pointers. */
  uint32_t (*entity_handle_of)(ru_plugin* self, void* entity);
  /* Designer name ("cs_player_controller", "weapon_ak47"), or NULL. */
  const char* (*entity_classname)(ru_plugin* self, void* entity);
  /* Marks the whole entity dirty so fields written after its first snapshot are re-sent. 1 = done. */
  int (*entity_mark_changed)(ru_plugin* self, void* entity);
  /* CAttributeList::SetOrAddAttributeValueByName on an attribute list inside an entity. 1 = done. */
  int (*econ_attr_set_by_name)(ru_plugin* self, void* attribute_list, const char* name, double value);
  /* CBaseEntity::ChangeSubclass (knives: the item defindex as a string, "507"). 1 = done. */
  int (*entity_change_subclass)(ru_plugin* self, void* entity, const char* subclass);
  /* CBaseModelEntity::SetModel("agents/models/....vmdl"). 1 = done. */
  int (*entity_set_model)(ru_plugin* self, void* entity, const char* model);
  /* Bodygroup by name on the entity's current model. Returns ru_bodygroup_result. */
  int (*entity_set_bodygroup_by_name)(ru_plugin* self, void* entity, const char* group, int value);

  /* -- match control -- */

  /*
   * Suppress CS2 round termination (warmup / practice free play) through the core's
   * TerminateRound detour. 1 = the requested state is in effect. Forcing a round end is not
   * part of the API: the core has no verified way to call TerminateRound.
   */
  int (*set_round_termination_suppressed)(ru_plugin* self, int suppress);
  /*
   * Chat name prefix for one player (e.g. "[CAP]" with CS2 color bytes). Their chat lines are
   * relayed as "<prefix> <name>: <msg>" instead of the original line. NULL or "" clears it.
   * Removed when the plugin unloads. Takes precedence over the core's admin/captain prefix.
   */
  int (*set_chat_name_prefix)(ru_plugin* self, uint64_t steamid64, const char* prefix);

  /* -- admins -- */

  /* Any thread; may block (DB lookup). 1 = admin. Asks the provider first, then the core. */
  int (*is_admin)(ru_plugin* self, uint64_t steamid64);
  /* Registers the admin provider (one at a time; NULL clears it). Removed on unload. */
  int (*set_admin_provider)(ru_plugin* self, ru_admin_provider_fn fn, void* user);

  /* -- config -- */

  /*
   * Plugin config value: `key = value` from cfg/ReadyUp/<plugin>.cfg (csgo/cfg; top-level keys
   * or a `[<plugin>]` section), then the `[<plugin>]` section of readyup.cfg. Returns the value length (truncated to len-1 in
   * buf), or -1 if the key is not set.
   */
  int (*config_get)(ru_plugin* self, const char* key, char* buf, uint32_t len);
  /* Any thread. readyup.cfg debug=1. */
  int (*debug_enabled)(ru_plugin* self);
  /* Any thread. Absolute directory holding readyup.cfg (the core's dir). */
  const char* (*config_dir)(ru_plugin* self);

  /* -- plugin-to-plugin -- */

  /*
   * Publishes an interface (a pointer to a plugin-defined C struct of function pointers)
   * under a name such as "readyup.match.v1". One provider per name. Removed on unload.
   */
  int (*provide_interface)(ru_plugin* self, const char* name, uint32_t version, void* iface);
  /*
   * Looks one up (NULL if missing or older than min_version). The pointer is only valid
   * until the provider can unload, i.e. until your callback returns: look it up again in
   * every callback instead of keeping it.
   */
  void* (*get_interface)(ru_plugin* self, const char* name, uint32_t min_version);

  /* -- state across reloads -- */

  /*
   * Keep a byte blob in core memory, keyed by (plugin name, key), across an unload and the
   * next load of the same plugin (e.g. `ru plugin reload`). Not persisted to disk. len 0
   * deletes it. 1 = stored. Max 1 MiB per blob.
   */
  int (*stash_put)(ru_plugin* self, const char* key, const void* data, uint32_t len);
  /* Copies up to cap bytes into buf; returns the blob's full size, or -1 if there is none. */
  int (*stash_get)(ru_plugin* self, const char* key, void* buf, uint32_t cap);

  /* ==== v1.2 ============================================================
   * Appended in 1.2. Require 1.2 in ru_plugin_info.api_version, or check RU_API_HAS().
   */

  /* -- output -- */

  /*
   * Any thread. Like `log`, but without the `plugin[<name>]: ` tag: the line reads
   * `[ReadyUp] <msg>` (`[ReadyUp] [dbg] <msg>` for RU_LOG_DEBUG). For plugins that own log
   * formats operators and tools already parse (match: `state: ...`, `knife: ...`).
   */
  void (*log_untagged)(ru_plugin* self, int level, const char* msg);

  /* -- commands -- */

  /* register_chat_command with RU_CMD_* flags (RU_CMD_HIDE). */
  ru_handle (*register_chat_command_ex)(ru_plugin* self, const char* name, uint32_t flags, ru_command_fn fn,
                                        void* user);
  /* register_console_command with RU_CMD_* flags (RU_CMD_OBSERVE). */
  ru_handle (*register_console_command_ex)(ru_plugin* self, const char* name, uint32_t flags, ru_command_fn fn,
                                           void* user);
  /*
   * A subcommand of `ru`: `ru <name> ...` on the server console / RCON and `.ru <name> ...` in
   * chat both call fn. argv[0] is "ru" (console) or ".ru" (chat), argv[1] is the subcommand.
   * name is [a-z0-9_-]; the core's own subcommands (help, plugin, version, selftest, sigtest,
   * reload, status_http) and another plugin's are refused. Chat senders are not checked: use
   * is_admin. ctx->slot is the sender's slot when known.
   */
  ru_handle (*register_ru_subcommand)(ru_plugin* self, const char* name, ru_command_fn fn, void* user);

  /* -- frames -- */

  /*
   * Called on EVERY GameFrame, simulating or not (tick->simulating tells which), after the
   * core's frame work, queued commands / events / log lines. For timers that must also fire
   * while the server does not simulate (map changes, an emptied server). on_tick is the
   * simulating-only variant.
   */
  ru_handle (*on_frame)(ru_plugin* self, ru_tick_fn fn, void* user);

  /* -- engine capabilities -- */

  /*
   * State of a core feature or engine dependency, as `ru selftest` shows it: 1 = on / verified,
   * 0 = pending (not decided yet, e.g. before the first map), -1 = off / failed / unknown name.
   * Names: the feature table (`match_flow`, `knife`, `pauses`, `ready_hud`, `welcome_html`,
   * `hud_brand`, `chat_commands`, `player_chat_print`, ...), dependencies (`fn:<engine function>`,
   * `vtable:<slot>`, `layout:<struct>`, `hook:GameFrame`, `hook:ClientCommand`, `cmdbuf`,
   * `loglistener`, `schema`, `entsys`, `eventmgr`) and `events_live`: engine game events are
   * delivered and drive the round lifecycle (0 while the core still derives it from log lines).
   */
  int (*feature_state)(ru_plugin* self, const char* name);

  /* Current map ("de_dust2"), "" before the first map. Valid until the callback returns. */
  const char* (*current_map)(ru_plugin* self);

  /* ==== v1.3 ============================================================
   * Appended in 1.3. Require 1.3 in ru_plugin_info.api_version, or check RU_API_HAS().
   */

  /*
   * Moves an entity to origin[0..2] (x, y, z) with CBaseEntity::SetAbsOrigin, the call CS2's
   * setpos / setpos_player / ent_setpos make (those commands need a client of their own and do
   * nothing from the server console). On a player pawn this is a teleport; velocity and view
   * angles are left alone. Game thread. 1 = moved, 0 = unavailable (engine surface
   * CBaseEntity_SetAbsOrigin unresolved) or a non-finite / off-map origin.
   */
  int (*entity_set_abs_origin)(ru_plugin* self, void* entity, const float* origin);

  /* v1.4+: fields are appended here. Check RU_API_HAS() before use. */
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
