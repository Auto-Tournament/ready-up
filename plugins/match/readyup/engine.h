#pragma once

// Engine-facing helpers of readyup-match, all backed by ru_api (host.cpp). They keep the names
// the match flow used while it was compiled into the core; the core's own versions of these
// (chat.h, client_print.h, center_html.h, command_buffer_hook.h, features.h, ...) are the real
// engine code behind ru_api.
//
// Game thread unless noted. Output/command helpers called from a worker thread are queued and
// sent on the next frame (host::RunOnGameThread) and report success.

#include <cstdint>
#include <optional>
#include <string>

namespace readyup {

// ---- chat (ru_api chat_all / chat_to_slot) ----------------------------------------------------

// Chat to everyone with the Ready Up prefix. Any thread.
void SendToChat(const char* msg);
// Chat to everyone, no prefix (payload may carry CS2 color bytes). Any thread.
void SendRawToChat(const char* payload);
// Debug chat (readyup.cfg chat_debug=1 and debug=1 only). Any thread.
void AnnounceToChat(const char* msg);
// Chat to one player slot. Any thread (queued: then always true).
bool ClientPrintChat(int slot, const char* msg);
// True if broadcast chat goes out without the "Console:" prefix (UTIL_ClientPrintAll resolved).
bool ClientPrintAvailable();

// ---- center HTML (ru_api center_html_to_slot) --------------------------------------------------

// Per-client center HTML panel. Game thread only.
bool PrintCenterHtmlToClientOnly(int slot, const std::string& html, int durationSeconds);

// ---- server commands (ru_api server_command) ---------------------------------------------------

// Queue a server console command. Any thread.
bool EnqueueServerCommand(const char* text);

// ---- core features (ru_api feature_state) ------------------------------------------------------

enum class Feature : int {
  ChatCommands = 0,
  MatchFlow,
  Pauses,
  WelcomeHtml,
  RoundTermSuppression,
  Events,
  ClientCommandHook,
  PlayerChatPrint,
  ReadyHud,
  HudBrand,
  Knife,
  Plugins,
  kCount
};
const char* FeatureName(Feature f);
// True while the core reports the feature on. Any thread (cached per frame off the game thread).
bool FeatureEnabled(Feature f);

// ---- round termination (ru_api set_round_termination_suppressed) -------------------------------

void SetRoundTerminationSuppressed(bool suppress);
bool RoundTerminationSuppressed();

// Forcing a client onto a team is not part of the verified engine surface (see the core's
// server_game_clients_hook.h). Always false; callers handle that.
inline bool ForceJoinTeamForSlot(int /*slot*/, int /*joinTeam*/) { return false; }

// Ready Up never runs its match flow while the core is disabled (no plugin loads then).
inline bool IsDisabled() { return false; }

// ---- paths / versions ----------------------------------------------------------------------------

// The core's directory (csgo/readyup/bin/linuxsteamrt64: readyup.cfg). Any thread.
std::string GetThisModuleDir();
// csgo/. Any thread.
std::string GetCsgoDirFromModuleDir();
// Release version of this plugin (same as the core's for bundled builds), e.g. "0.1.0".
const char* SemVer();
// The core's build string, e.g. "0.1.0 (abc1234)". Any thread.
const char* BuildVersion();

}  // namespace readyup
