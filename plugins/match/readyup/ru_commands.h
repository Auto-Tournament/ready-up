#pragma once

// The match plugin's `ru` commands as main commands with subcommands (`.ru <main> <sub> [args]`
// in chat, `ru <main> <sub> [args]` on the console / RCON), and their help:
//
//   .ru help                 main commands (core, ru_router.cpp)
//   .ru help <main>          its subcommands; the core forwards it here as `.ru <main> help`
//   .ru <main>               the same help
//   .ru <main> <unknown>     "unknown command, type .ru help <main>"
//
// Pure logic, no engine calls (ctest `match_ru_commands`); the handlers are match_router.cpp.
// Admin-only subcommands need IsReadyUpAdmin (the server console always may).

#include <string>
#include <vector>

namespace readyup {

struct RuSubcommand {
  const char* name;
  const char* args;  // "" = none
  const char* help;
  bool admin;
};

struct RuMainCommand {
  const char* name;
  const char* help;
  std::vector<RuSubcommand> subs;
};

// In display order: match, map, mode, admins, hud.
const std::vector<RuMainCommand>& MatchRuCommands();
const RuMainCommand* FindRuMain(const std::string& name);
const RuSubcommand* FindRuSub(const RuMainCommand& main, const std::string& sub);

// `.ru help <main>` lines: a header, then one line per subcommand (".ru match load <url>: load a
// match config (admin)").
std::vector<std::string> RuHelpLines(const RuMainCommand& main);
// The reply to `.ru <main> <sub>` with an unknown sub.
std::string RuUnknownSubReply(const std::string& main, const std::string& sub);

// Arguments after `map change`: one map name or workshop id ("3084291314", "ws:3084291314",
// "workshop/3084291314[/name]"; map_names.h). False and *err (usage or reason) otherwise.
bool ParseMapChange(const std::vector<std::string>& args, std::string* entry, std::string* err);
// Arguments after `match load`: one http(s) URL. False and *err otherwise.
bool ParseMatchLoad(const std::vector<std::string>& args, std::string* url, std::string* err);

}  // namespace readyup
