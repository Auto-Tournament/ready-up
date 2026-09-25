#pragma once

// In-game admin commands of the match plugin (`.ru <cmd>` in chat, `ru <cmd>` on the console):
// the parsing and the help table. Pure logic, no engine calls (ctest `match_admin_commands`); the
// handlers are in match_router.cpp. Every command here is admin-only (IsReadyUpAdmin; the server
// console always may).
//
//   .ru map <name|workshop id>   changelevel <name> / host_workshop_map <id> (map_names.h entries)
//   .ru reloadmap                load the current map again (workshop maps by their id)
//   .ru restart                  mp_restartgame 1 (the match, if any, stays loaded)
//   .ru load <url>               load a match config (= ru match load <url>)
//   .ru end                      end the loaded match (series_end winner none) and reset
//   .ru match restart            the loaded match back to its warmup (players ready again)

#include <string>
#include <vector>

namespace readyup {

struct AdminCommandHelp {
  const char* usage;
  const char* help;
};
// For `.ru help` / `ru help`, in display order.
const std::vector<AdminCommandHelp>& AdminCommandsHelp();
// One chat line: ".ru map <name|id> | .ru reloadmap | ...".
std::string AdminCommandsChatLine();

// Arguments after `map`. True and *entry (a map_names.h entry) for one valid map name or workshop
// id ("3084291314", "ws:3084291314", "workshop/3084291314[/name]"); else false and *err (a usage
// or a reason, for the reply).
bool ParseMapCommand(const std::vector<std::string>& args, std::string* entry, std::string* err);

// Arguments after `load`: one http(s) URL. False and *err otherwise.
bool ParseLoadCommand(const std::vector<std::string>& args, std::string* url, std::string* err);

}  // namespace readyup
