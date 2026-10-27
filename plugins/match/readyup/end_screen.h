#pragma once

// The leaderboard at the end of a map or series (hud.so's at_kit.xml #at_end, the Auto Tournament
// HUD addon). Design: canvas "Auto Tournament HUD kit", page In game (v3), "Leaderboard v4".
//
// Each player sees it from their own side: "You won!" / "You lost" / "Draw" (spectators: who won),
// their row marked, their team's rounds in the accent on the round strip. The world is blurred and
// the cursor is held while it is up, so the column heads explain themselves on hover.

#include "readyup/match_stats.h"

#include <string>

namespace readyup {

struct EndScreenInfo {
  std::string team1, team2;  // names
  int score1 = 0, score2 = 0;  // the map score, or the series score when the series is over
  std::string winner;          // "team1" | "team2" | "" (draw)
  std::string how;             // "By rounds", "In overtime", "By damage", "By maps", "By forfeit", ""
  std::string where;           // footer left: "MAP 2 OF 3 · INFERNO"
  std::string nextLabel;       // footer right: "Next map:" / "Server closes"
  std::string next;            // "Mirage", or ""
  int seconds = 30;            // how long it stays up (the footer counts down)
  int maxRounds = 24;          // regulation, for the halftime gaps on the round strip
  int overtimeRounds = 6;      // rounds per overtime
  stats::MapStats stats;       // the map just played
};

// The rows, the strip and the awards from the map's stats (no engine calls).
struct EndScreenRow {
  std::string name;
  uint64_t id = 0;
  int k = 0, d = 0, a = 0, adr = 0, kast = 0, hs = 0, ud = 0, ek = 0, mvps = 0;
};
struct EndScreenAward {
  std::string label, name, value;
};
struct EndScreenRound {
  int winner = 0;    // team slot 1 / 2
  char how = 'E';    // E elimination, B bomb, D defuse, T time
  bool halfBefore = false;
};
struct EndScreenData {
  std::vector<EndScreenRow> team1, team2;  // best first, at most 5 each
  std::vector<EndScreenAward> awards;      // up to 4
  std::vector<EndScreenRound> rounds;      // at most 30
  std::string sides1, sides2;              // "T 8 · CT 5"
};
EndScreenData BuildEndScreenData(const EndScreenInfo& info);

// Shows it to everyone on the server. False when the HUD can't draw it (then the result card).
bool EndScreenShow(const EndScreenInfo& info);
void EndScreenHide();
// Once a tick (result_card.cpp): the countdown, and hiding it when its time is up.
void EndScreenTick();
bool EndScreenActive();

}  // namespace readyup
