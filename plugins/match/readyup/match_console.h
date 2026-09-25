#pragma once

// readyup-match console / RCON commands (see match_console.cpp).

#include <string>
#include <vector>

namespace readyup {

// The command names the plugin registers (ru_api register_console_command).
const std::vector<std::string>& MatchConsoleCommands();

// One console line whose first token is one of MatchConsoleCommands() (or `tv_delay`, observed).
// Returns true if it was handled.
bool MatchConsoleCommand(const std::string& line);

// `ru match load <url>`: fetch the match config (blocking, short timeouts) and load it.
bool LoadMatchFromUrl(const std::string& url);

}  // namespace readyup
