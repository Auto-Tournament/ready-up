#pragma once

#include <cstdint>
#include <string>

namespace readyup {

// Per-player "ReadyUp is live" welcome screen (center HTML), shown the first time a
// player lands on T or CT on the current map.
//
// Triggers (any of them; deduped per player):
// - engine log line `"Name<slot><steam>" switched from team <X> to <CT|TERRORIST>`
//   (in-process logging listener and/or the console-log tailer in log_receiver.cpp)
// - IServerGameClients::ClientCommand `jointeam 2|3` (tentative; a confirmed
//   source arriving before display overrides the team)
// - `player_team` game event (when engine events are available)
//
// Policy: shown once per player per map (keyed by SteamID64 when known, else slot).
// A later team switch does not re-show it; if the switch happens while the screen
// is still up, the team line updates in place. A `Loading map`/`Started map` log
// line resets the policy.
//
// Toggle: readyup.cfg `welcome=1` (default) / env READYUP_WELCOME=0.

enum class WelcomeSource {
  LogLine,
  ClientCommand,
  GameEvent,
};

// Thread-safe; only records intent. Never touches the engine.
// team: 2=T, 3=CT (anything else is ignored).
void WelcomeObserveTeamJoin(int slot, int team, uint64_t steamid64, const std::string& name, WelcomeSource src);

// Thread-safe; parses team-switch / disconnect log lines. Cheap for other lines.
void WelcomeObserveLogLine(const std::string& line);

// GameFrame thread only: sends/refreshes pending welcome screens.
void WelcomeTick();

// Thread-safe: true while a welcome screen is being shown to this player (used to
// keep the warmup banner from overwriting it).
bool WelcomeActiveForSteam(uint64_t steamid64);

}  // namespace readyup
