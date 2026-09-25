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

}  // namespace readyup
