#pragma once

// `ru help` / `.ru help` text: main commands, and `help <main>` for the core's own. Pure (ctest
// `ru_help`). Plugin main commands answer `help <main>` themselves: the core forwards it to the
// plugin as `ru <main> help`.

#include <string>
#include <vector>

namespace readyup {

// The core's main commands for chat (`.ru <main>`), in display order.
const std::vector<std::string>& CoreRuMainCommands();

// `.ru help`: a header, then one line per main command (sorted, no duplicates): the core's with
// what they do, the plugins' with the plugin that owns them. `pluginMains` entries are
// "name (plugin)" as plugins::PluginRuSubcommands() lists them (or a bare name). Chat has no
// newlines, so each line is sent as its own message.
std::vector<std::string> RuMainHelpLines(const std::vector<std::string>& pluginMains);

// `.ru help <main>` for a core main command. Empty for anything else.
std::vector<std::string> CoreRuSubHelpLines(const std::string& main);

// The reply to `.ru <unknown>`.
std::string RuUnknownCommandReply(const std::string& cmd);

// Old plugin (MatchZy Enhanced / Auto Tournament CS2) names the core answers as one of its own
// `.ru <sub>` commands; "" for anything else (any case):
//   chat     .ruversion, .version  -> version   (public, like `.ru version`)
//            .reload_config        -> reload    (admin, like `.ru reload`)
//   console  ru_version            -> version   (was matchzy_version / at_version)
//            ru_reload_config      -> reload    (was matchzy_reload_config / at_reload_config)
std::string CoreChatAliasToRu(const std::string& firstToken);
std::string CoreConsoleAliasToRu(const std::string& firstToken);

// ---- who may run `.ru ...` ---------------------------------------------------------------------
// Every `.ru <sub>` a player sends is admin-only (default deny), checked by the core router before
// the core or a plugin sees it; the server console / RCON always may. Open to every player:
//   .ru | .ru version | .ru help | .ru list
//   .ru help <main> for a public main below (.ru help dm)
//   a few plugin commands meant for players, kRuPublicPluginCommands in ru_help_text.cpp
//   (.ru dm top | status | hud: deathmatch has no player chat commands).
// Plugins still check admin themselves for their admin subcommands (defense in depth).

// True when `words` (the words after `.ru`, any case) are open to every player.
bool RuCommandPublic(const std::vector<std::string>& words);
// True when a player may run `.ru <words...>`: public, or isAdmin.
inline bool RuCommandAllowed(const std::vector<std::string>& words, bool isAdmin) {
  return isAdmin || RuCommandPublic(words);
}
// `.ru help` / `.ru list` for a player who is not an admin: only what they can run.
// `pluginMains` as for RuMainHelpLines.
std::vector<std::string> RuPublicHelpLines(const std::vector<std::string>& pluginMains);

}  // namespace readyup
