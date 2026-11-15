#pragma once
// Kills to the platform: every `killed` line of a live match goes out as the fleet event `event.kill`
// (plugins/fleet/protocol/v1/messages/event.kill.json), from the server log (bots have no Steam ID, so
// the log, which names them, is the source). The engine-free half: the parser and the event's `data`.
// ctest `match_kill_forward`.
#include "readyup/status_snapshot.h"

#include <cstdint>
#include <optional>
#include <string>

namespace readyup::kill_forward {

struct Fighter {
  std::string name;
  uint64_t steamid64 = 0;  // 0 for a bot
  std::string side;        // "ct" | "t" | "spec"
  double x = 0, y = 0, z = 0;
};

struct Kill {
  Fighter attacker;
  Fighter victim;
  std::string weapon;
  bool headshot = false;
};

// `L .. : "A<3><BOT><CT>" [x y z] killed "B<8><[U:1:123]><TERRORIST>" [x y z] with "ak47" (headshot)`.
// nullopt for anything else: `killed other` (chickens), suicides, the bomb, a line with more than one
// ` killed "`.
std::optional<Kill> ParseKillLogLine(const std::string& line);

// gameTime: the server's curtime when the line came in (NaN = unknown, the field is left out).
status::Json EventData(const Kill& k, long long atUnixMs, double gameTime);

}  // namespace readyup::kill_forward
