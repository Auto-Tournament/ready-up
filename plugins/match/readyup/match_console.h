#pragma once

// readyup-match console / RCON commands (see match_console.cpp).

#include "readyup/webhook.h"

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

// Loads a parsed match: closes a previous match's webhooks, sets the context, enters warmup,
// persists `configJson` for recovery, kicks bots, turns on CS2 round backups
// (readyup_backup_<matchid>_map1_*) and changes to map 1 (`changelevel <maplist[0]>` unless
// already there, or `map1Command` when given, e.g. `host_workshop_map <id>`). Game thread.
// Used by `ru match load` and by the fleet link's match.assign (fleet_bridge.cpp).
void ApplyLoadedMatch(const WebhookMatchContext& ctx, const std::string& configJson,
                      const std::string& map1Command = {});

}  // namespace readyup
