#pragma once

// readyup-core plugin host: loads csgo/readyup/plugins/<name>.so, hands each one the
// C function table from core/include/readyup/plugin_api.h and owns every registration a
// plugin makes, so unloading never leaves anything behind. See docs/ARCHITECTURE.md.

#include "readyup/plugin_api.h"

#include <cstdint>
#include <string>
#include <vector>

namespace readyup::plugins {

// A normalized lifecycle event (see ru_event in core/include/readyup/plugin_api.h).
struct LifecycleEvent {
  uint32_t type = 0;    // ru_event_type
  uint32_t source = 0;  // ru_event_source
  int slot = -1;
  uint64_t steamid64 = 0;
  std::string name;
  std::string map;  // MAP_START: the new map; otherwise filled in by PostEvent
  int team = 0;
  int old_team = 0;
  int winner = 0;
  int reason = 0;
  int round = 0;
  int team_ct_score = -1;
  int team_t_score = -1;
};

// Thread-safe. Queues an event for delivery on the next GameFrame. Cheap no-op when no
// plugin subscribed to the type.
void PostEvent(LifecycleEvent ev);

// Thread-safe. If a loaded plugin owns the chat command in `text` (first token), queues
// the call for the next GameFrame and returns true. Real players only (steamid64 != 0).
// `slot` is the sender's engine slot when the caller knows it (ClientCommand), else -1.
bool TryDispatchChat(uint64_t steamid64, const std::string& playerName, const std::string& text, int slot = -1);

// Thread-safe. True if a loaded plugin owns the chat command `token` (lowercase first token,
// e.g. ".r"); *flags gets its RU_CMD_* flags (RU_CMD_HIDE: the caller swallows the line).
bool ChatCommandOwned(const std::string& token, uint32_t* flags);

// Thread-safe. `ru <sub> ...` (console, steamid64 0) or `.ru <sub> ...` (chat): if a plugin
// registered <sub> (register_ru_subcommand), queues the call and returns true.
bool TryDispatchRu(bool console, uint64_t steamid64, const std::string& playerName, const std::string& text,
                   int slot = -1);

// Subcommand names plugins registered, as "<sub> (<plugin>)", for `ru help`. Thread-safe.
std::vector<std::string> PluginRuSubcommands();

// True if `sub` (lowercase) is a subcommand the core handles itself; plugins cannot take it.
// Implemented in ru_router.cpp.
bool IsCoreRuSubcommand(const std::string& sub);

// Thread-safe. Queues the line for RU_CMD_OBSERVE console registrations of its first token
// (the line itself is not consumed). Called for every console line the AddText hook sees.
void ObserveConsole(const std::string& line);

// Thread-safe. Same for a server console / RCON line. Only consulted after every core
// console handler declined the line, so core commands always win.
bool TryDispatchConsole(const std::string& line);

// Called from the GameFrame hook on every frame (simulating or not): first call loads
// every plugin in the plugins dir; then pending load/unload/reload requests run, then
// queued tasks / commands / events are delivered; per-tick callbacks only when simulating.
void Frame(bool simulating);

// `ru plugin list|load|unload|reload <name>` (console) and `.ru plugin ...` (chat admin).
// `args` excludes the leading "ru plugin". Output lines go to the console log and, when
// `replyToChat` is set, to chat. load/unload/reload run immediately when called on the
// game thread outside any plugin callback, otherwise at the start of the next GameFrame.
void HandlePluginCommand(const std::vector<std::string>& args, bool replyToChat);

// Name of the plugin whose code is running right now on the game thread ("" if none).
// Read by the crash handler; never null.
const char* CrashContextPlugin();

// For `ru selftest`. Thread-safe.
struct PluginHostStatus {
  std::string dir;
  unsigned apiMajor = 0, apiMinor = 0;
  bool started = false;        // first GameFrame ran (plugins dir scanned)
  bool disabledByEnv = false;  // READYUP_PLUGINS=0
  std::vector<std::string> loaded;    // "<name> <version>"
  std::vector<std::string> failures;  // "<name>: <error>" from the initial load
};
PluginHostStatus GetPluginHostStatus();

// Game thread. The core's own get_interface (e.g. the status endpoint reading
// readyup.fleet.v1): NULL if missing, older than minVersion, or its provider is unloading.
// Valid until the provider can unload, i.e. do not keep it past the current frame.
void* CoreGetInterface(const char* name, uint32_t minVersion);

// Any thread. Runs the `run` of every "readyup.selftest.<plugin>" interface a loaded plugin
// published (core/include/readyup/selftest_iface.h) and returns what they reported. A plugin
// being unloaded meanwhile stays mapped until its `run` returns.
struct PluginSelftestCheck {
  std::string plugin;
  std::string status;  // "OK" | "FAIL" | "PEND" | "SKIP" | "INFO" | "WARN"
  std::string name;
  std::string detail;
};
std::vector<PluginSelftestCheck> RunPluginSelftests();

// ---- API v1.1 hooks for the rest of the core ---------------------------------------

// Game thread, called from the engine's FireGameEvent: delivers the event synchronously to
// every plugin that subscribed to `name` (subscribe_game_event). `ev` is the engine's
// IGameEvent*, opaque to the loader. Cheap when nobody subscribed.
void DispatchGameEvent(const char* name, void* ev);

// Event names plugins subscribed to (for the core's listener registration) and a counter that
// changes whenever that set changes. Thread-safe.
std::vector<std::string> WantedGameEvents();
uint64_t WantedGameEventsGeneration();

// Thread-safe. Queues one server log line for subscribe_log_line subscribers.
void PostLogLine(const std::string& line);

// Thread-safe. Map of the last RU_EVENT_MAP_START ("" before the first).
std::string CurrentMap();

// Thread-safe. Chat name prefix a plugin set for this player (set_chat_name_prefix).
bool PluginChatPrefixFor(uint64_t steamid64, std::string* prefix);

// Any thread. Asks the admin provider a plugin registered (set_admin_provider).
// Returns -1 when there is none or it has no opinion, else 0/1.
int PluginAdminVerdict(uint64_t steamid64);

// Implemented in plugin_engine_api.cpp (the engine-facing members of ru_api); the offline
// host test links a stub instead.
namespace detail {
void FillEngineApi(ru_api* api);
// True if `self` is a live plugin and this is the game thread (logs and returns false
// otherwise). Every game-thread-only API member starts with this.
bool CheckGameThread(ru_plugin* self, const char* fn);
}  // namespace detail

}  // namespace readyup::plugins
