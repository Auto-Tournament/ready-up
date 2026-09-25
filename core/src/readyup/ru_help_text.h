#pragma once

// `ru help` / `.ru help` text: main commands, and `help <main>` for the core's own. Pure (ctest
// `ru_help`). Plugin main commands answer `help <main>` themselves: the core forwards it to the
// plugin as `ru <main> help`.

#include <string>
#include <vector>

namespace readyup {

// The core's main commands for chat (`.ru <main>`), in display order.
const std::vector<std::string>& CoreRuMainCommands();

// `.ru help`: one line with every main command (core + plugin `pluginMains`, sorted, no
// duplicates), then how to get the subcommands.
std::vector<std::string> RuMainHelpLines(const std::vector<std::string>& pluginMains);

// `.ru help <main>` for a core main command. Empty for anything else.
std::vector<std::string> CoreRuSubHelpLines(const std::string& main);

// The reply to `.ru <unknown>`.
std::string RuUnknownCommandReply(const std::string& cmd);

}  // namespace readyup
