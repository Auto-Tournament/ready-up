#pragma once

#include <cstdint>
#include <string>

namespace readyup {

// Best-effort match snapshot derived from console logs.
struct MatchStateSnapshot {
  int map_number = 1;
  int round_number = 0;
  int team1_score = 0;
  int team2_score = 0;
  std::string current_map;
};

// Thread-safe set/get for the current snapshot.
void MatchStateSetMap(int map_number, std::string map_name);
void MatchStateSetRound(int round_number);
void MatchStateSetScore(int team1_score, int team2_score);
MatchStateSnapshot MatchStateGet();

}  // namespace readyup

