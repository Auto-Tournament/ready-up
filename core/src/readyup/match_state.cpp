#include "readyup/match_state.h"

#include <mutex>

namespace readyup {
namespace {

std::mutex g_mu;
MatchStateSnapshot g_state;

}  // namespace

void MatchStateSetMap(int map_number, std::string map_name) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (map_number >= 1) g_state.map_number = map_number;
  g_state.current_map = std::move(map_name);
}

void MatchStateSetRound(int round_number) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (round_number >= 0) g_state.round_number = round_number;
}

void MatchStateSetScore(int team1_score, int team2_score) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_state.team1_score = team1_score;
  g_state.team2_score = team2_score;
}

MatchStateSnapshot MatchStateGet() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_state;
}

}  // namespace readyup

