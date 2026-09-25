/*
 * readyup-hello: the smallest useful Ready Up plugin, written in plain C to prove
 * the ABI. It exists to exercise load / unload / reload:
 *
 *   .hello            (chat)     replies to the sender and to everyone
 *   hello_status      (console)  prints load generation, tick and log-line counts
 *   every 10 s                   logs a heartbeat with the frame counter
 *   lifecycle events             logged (map/round/player)
 *
 * With a v1.1 core it also shows the newer members (each guarded by RU_API_HAS, so the
 * plugin still loads on a 1.0 core):
 *   stash_put/get     a load counter that survives `ru plugin reload hello`
 *   config_get        `greeting` from cfg/ReadyUp/hello.cfg or readyup.cfg [hello]
 *   raw game events   player_death: attacker slot + weapon
 *   log lines         counted, shown by hello_status
 *   interfaces        publishes "readyup.hello.v1"
 * and the 1.2 ones:
 *   `ru hello`        (console) / `.ru hello` (chat): a plugin ru subcommand, logged untagged
 *   .hellohide        (chat)     hidden from chat (RU_CMD_HIDE), answers the sender only
 *   on_frame          frames counted (simulating or not), shown by hello_status
 *   sv_cheats         observed on the console (RU_CMD_OBSERVE; the engine still runs it)
 *   current_map       shown by hello_status
 *
 * Bump HELLO_VERSION, rebuild, `ru plugin reload hello`: the new string shows up
 * without restarting the server.
 */
#include "readyup/plugin_api.h"

#include <stdio.h>
#include <string.h>

#ifndef HELLO_VERSION
#define HELLO_VERSION "1.2.0"
#endif

static const ru_api* g_api;
static double g_lastBeat;
static unsigned long long g_ticks;
static unsigned g_hellos;
static unsigned g_loads;
static unsigned long long g_logLines;
static unsigned long long g_frames;
static char g_greeting[64];

/* What other plugins get from get_interface("readyup.hello.v1", 1). */
typedef struct hello_iface_v1 {
  uint32_t struct_size;
  unsigned (*greetings)(void);
} hello_iface_v1;

static unsigned Greetings(void) { return g_hellos; }
static const hello_iface_v1 g_iface = {sizeof(hello_iface_v1), Greetings};

static void OnHello(void* user, const ru_command_ctx* ctx) {
  (void)user;
  char msg[256];
  ++g_hellos;
  snprintf(msg, sizeof(msg), "%s, %s! (readyup-hello " HELLO_VERSION ", greeting #%u)", g_greeting, ctx->name,
           g_hellos);
  if (ctx->slot < 0 || !g_api->chat_to_slot(g_api->self, ctx->slot, msg)) {
    g_api->chat_all(g_api->self, msg, 0);
  }
  ru_logf(g_api, RU_LOG_INFO, ".hello from %s (steamid64=%llu slot=%d argc=%d)", ctx->name,
          (unsigned long long)ctx->steamid64, ctx->slot, ctx->argc);
}

static void OnStatus(void* user, const ru_command_ctx* ctx) {
  (void)user;
  (void)ctx;
  const char* map = RU_API_HAS(g_api, current_map) ? g_api->current_map(g_api->self) : "?";
  ru_logf(g_api, RU_LOG_INFO,
          "status: version " HELLO_VERSION ", load #%u, %llu ticks, %llu frames, %u greetings, %llu log lines, map %s, core %s",
          g_loads, g_ticks, g_frames, g_hellos, g_logLines, map[0] ? map : "-", g_api->core_version);
}

/* v1.2: `ru hello` / `.ru hello`, logged without the plugin tag. */
static void OnRuHello(void* user, const ru_command_ctx* ctx) {
  (void)user;
  char msg[256];
  snprintf(msg, sizeof(msg), "hello-ru: %s via %s (argc=%d, slot=%d)", ctx->argc > 2 ? ctx->argv[2] : "-",
           ctx->is_console ? "console" : "chat", ctx->argc, ctx->slot);
  g_api->log_untagged(g_api->self, RU_LOG_INFO, msg);
}

/* v1.2: hidden chat command; the answer goes to the sender only. */
static void OnHelloHide(void* user, const ru_command_ctx* ctx) {
  (void)user;
  if (ctx->slot >= 0) g_api->chat_to_slot(g_api->self, ctx->slot, "psst (only you saw that)");
}

static void OnObserveCheats(void* user, const ru_command_ctx* ctx) {
  (void)user;
  ru_logf(g_api, RU_LOG_INFO, "observed console: %s", ctx->text);
}

static void OnFrame(void* user, const ru_tick_info* t) {
  (void)user;
  (void)t;
  ++g_frames;
}

static void OnTick(void* user, const ru_tick_info* t) {
  (void)user;
  ++g_ticks;
  if (g_lastBeat == 0.0) g_lastBeat = t->now;
  if (t->now - g_lastBeat >= 10.0) {
    g_lastBeat = t->now;
    ru_logf(g_api, RU_LOG_INFO, "tick heartbeat: frame=%llu plugin_ticks=%llu", (unsigned long long)t->frame, g_ticks);
  }
}

static const char* EventName(uint32_t type) {
  switch (type) {
    case RU_EVENT_MAP_START: return "map_start";
    case RU_EVENT_MATCH_START: return "match_start";
    case RU_EVENT_ROUND_START: return "round_start";
    case RU_EVENT_ROUND_END: return "round_end";
    case RU_EVENT_PLAYER_CONNECT: return "player_connect";
    case RU_EVENT_PLAYER_DISCONNECT: return "player_disconnect";
    case RU_EVENT_PLAYER_TEAM: return "player_team";
    default: return "?";
  }
}

static void OnEvent(void* user, const ru_event* ev) {
  (void)user;
  ru_logf(g_api, RU_LOG_INFO, "event %s (%s) map=%s slot=%d name=\"%s\" team=%d old=%d winner=%d round=%d",
          EventName(ev->type), ev->source == RU_SOURCE_ENGINE ? "engine" : "log", ev->map, ev->slot, ev->name,
          ev->team, ev->old_team, ev->winner, ev->round);
}

static void OnDeath(void* user, const char* name, const ru_game_event* ev) {
  (void)user;
  const ru_api* a = g_api;
  ru_logf(a, RU_LOG_INFO, "%s attacker=%d victim=%d weapon=%s headshot=%d", name,
          a->ev_get_player_slot(a->self, ev, "attacker"), a->ev_get_player_slot(a->self, ev, "userid"),
          a->ev_get_string(a->self, ev, "weapon", "?"), a->ev_get_int(a->self, ev, "headshot", 0));
}

static void OnLogLine(void* user, const char* line) {
  (void)user;
  (void)line;
  ++g_logLines;
}

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      /* Built against 1.1 but only requires 1.0: every 1.1 member is checked with RU_API_HAS. */
      (READYUP_PLUGIN_API_VERSION_MAJOR << 16) | 0,
      "hello",
      HELLO_VERSION,
      "Ready Up",
      "example plugin: .hello, hello_status, 10s tick heartbeat, lifecycle event log",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != READYUP_PLUGIN_API_VERSION_MAJOR) return 1;
  g_api = api;
  g_lastBeat = 0.0;
  g_ticks = 0;
  g_hellos = 0;
  g_logLines = 0;
  g_frames = 0;
  g_loads = 1;
  snprintf(g_greeting, sizeof(g_greeting), "hello");

  if (RU_API_HAS(api, stash_get)) {
    unsigned prev = 0;
    if (api->stash_get(api->self, "loads", &prev, sizeof(prev)) == (int)sizeof(prev)) g_loads = prev + 1;
    api->stash_put(api->self, "loads", &g_loads, sizeof(g_loads));
  }
  if (RU_API_HAS(api, config_get)) {
    char buf[64];
    if (api->config_get(api->self, "greeting", buf, sizeof(buf)) > 0) snprintf(g_greeting, sizeof(g_greeting), "%s", buf);
  }

  if (!api->register_chat_command(api->self, ".hello", OnHello, NULL)) return 2;
  api->register_console_command(api->self, "hello_status", OnStatus, NULL);
  api->on_tick(api->self, OnTick, NULL);
  api->subscribe(api->self, RU_EVENT_ANY, OnEvent, NULL);
  if (RU_API_HAS(api, subscribe_game_event)) api->subscribe_game_event(api->self, "player_death", OnDeath, NULL);
  if (RU_API_HAS(api, subscribe_log_line)) api->subscribe_log_line(api->self, OnLogLine, NULL);
  if (RU_API_HAS(api, provide_interface)) api->provide_interface(api->self, "readyup.hello.v1", 1, (void*)&g_iface);
  if (RU_API_HAS(api, current_map)) {
    api->register_ru_subcommand(api->self, "hello", OnRuHello, NULL);
    api->register_chat_command_ex(api->self, ".hellohide", RU_CMD_HIDE, OnHelloHide, NULL);
    api->register_console_command_ex(api->self, "sv_cheats", RU_CMD_OBSERVE, OnObserveCheats, NULL);
    api->on_frame(api->self, OnFrame, NULL);
  }
  ru_logf(api, RU_LOG_INFO, "loaded " HELLO_VERSION " (core %s, load #%u, greeting \"%s\")", api->core_version, g_loads,
          g_greeting);
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  /* Registrations are removed by the core; nothing else to release here. */
  ru_logf(g_api, RU_LOG_INFO, "unloading after %llu ticks, %u greetings", g_ticks, g_hellos);
  g_api = NULL;
}
