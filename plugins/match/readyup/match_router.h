#pragma once

// readyup-match commands (was the match part of the core's ru_router.cpp and
// command_buffer_hook.cpp). The core routes chat / console lines and hands these over through
// ru_api registrations (match_plugin.cpp):
//
//   player chat commands  .r .ready .ur .unready .notready .nr .pause .p .tech .unpause .up .gg
//                         .ff .forfeit .stay .switch .swap .ct .t .help .prac .tactics
//                         .bot .cbot .crouchbot .boost .crouchboost .nobots
//   ru subcommands        chat `.ru <sub>` and console `ru <sub>`: admins hudtest prac practice
//                         idle scrim state status mode match start pause fp forcepause unpause up
//                         fup forceunpause restart end recover side
//   console commands      ru_match_token, ru_webhook_url, ru_heartbeat_url, ru_admins_url, ... (match_console.cpp)

#include <cstdint>
#include <string>
#include <vector>

namespace readyup {

// Player chat command lines (first token is one of the player commands above).
void MatchChatCommand(uint64_t steamid64, const std::string& playerName, const std::string& text);

// `.ru <sub> ...` from chat (steamid64 != 0) or from the console (steamid64 == 0, replies go
// to the console only where the old console path did).
void MatchRuCommand(uint64_t steamid64, const std::string& playerName, const std::string& text);

// `ru <sub> ...` typed on the server console / RCON. Console-only behaviour (ru match load,
// ru mode, ru side, ...) first, everything else as MatchRuCommand from the console.
void MatchRuConsole(const std::string& line);

// Every player chat command / ru subcommand the plugin registers.
const std::vector<std::string>& MatchPlayerChatCommands();
const std::vector<std::string>& MatchRuSubcommands();

}  // namespace readyup
