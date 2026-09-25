#pragma once

// readyup-match commands (was the match part of the core's ru_router.cpp and
// command_buffer_hook.cpp). The core routes chat / console lines and hands these over through
// ru_api registrations (match_plugin.cpp):
//
//   player chat commands  .r .ready .ur .unready .notready .nr .pause .p .tech .unpause .up .gg
//                         .ff .forfeit .stay .switch .swap .ct .t .help .prac .tactics
//                         .bot .cbot .crouchbot .boost .crouchboost .nobots
//   ru main commands      chat `.ru <main> <sub>` and console `ru <main> <sub>`: match, map,
//                         mode, admins, hud (subcommands and help: ru_commands.h)
//   console commands      ru_match_token, ru_webhook_url, ru_heartbeat_url, ru_admins_url, ... (match_console.cpp)

#include <cstdint>
#include <string>
#include <vector>

namespace readyup {

// Player chat command lines (first token is one of the player commands above).
void MatchChatCommand(uint64_t steamid64, const std::string& playerName, const std::string& text);

// `.ru <main> <sub> ...` from chat (steamid64 != 0; slot: the sender's, -1 if unknown) or from
// the console (steamid64 == 0, replies go to the console).
void MatchRuCommand(uint64_t steamid64, const std::string& playerName, const std::string& text, int slot);

// `ru <main> <sub> ...` typed on the server console / RCON: MatchRuCommand from the console.
void MatchRuConsole(const std::string& line);

// Every player chat command / ru main command the plugin registers.
const std::vector<std::string>& MatchPlayerChatCommands();
const std::vector<std::string>& MatchRuSubcommands();

}  // namespace readyup
