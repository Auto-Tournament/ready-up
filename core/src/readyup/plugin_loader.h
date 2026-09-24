#pragma once

// readyup-core plugin host: loads csgo/readyup/plugins/<name>.so, hands each one the
// C function table from core/include/readyup/plugin_api.h and owns every registration a
// plugin makes, so unloading never leaves anything behind. See docs/ARCHITECTURE.md.

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
bool TryDispatchChat(uint64_t steamid64, const std::string& playerName, const std::string& text);

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

}  // namespace readyup::plugins
