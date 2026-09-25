#pragma once

// readyup-practice pure logic (ctest `practice_rules`): which chat commands are practice tools,
// when they may run, and when a dedicated practice server (always=1) switches practice on.

#include <string>

namespace practice {

// .rethrow .rt .savepos .loadpos .back .clear .noflash .god .spawn .ctspawn .tspawn
bool IsToolCommand(const std::string& cmd);
// .bot .cbot .crouchbot .boost .crouchboost .nobots
bool IsBotCommand(const std::string& cmd);

// Tools and bots run only in practice mode, and never under the valve ruleset (not in Valve's
// rulebook; docs/ESPORTS-MODE.md).
bool ToolsAllowed(bool practiceActive, const std::string& ruleset);

// A match plugin mode in which practice may not start (a match or scrim is under way).
bool MatchBlocksPractice(const std::string& ruMode);

// always=1 (dedicated practice server): switch practice on when it is off and nothing blocks it.
bool ShouldAutoEnter(bool always, bool practiceActive, const std::string& ruMode);

// "1"/"true"/"yes"/"on" -> true, "0"/"false"/"no"/"off" -> false, else def.
bool ParseBool(const std::string& text, bool def);

// The `.help` line while practice is on.
const char* HelpLine();

}  // namespace practice
