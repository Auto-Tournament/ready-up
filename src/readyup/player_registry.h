#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace readyup {

struct PlayerInfo {
  uint64_t steamid64 = 0;
  std::string name;
};

// Observations come from log parsing (best-effort).
void ObservePlayer(uint64_t steamid64, const std::string& name);

// Finds the closest match for a name fragment among observed players.
// Returns nullopt and sets `err` when no match or ambiguous.
std::optional<PlayerInfo> ClosestPlayerMatch(const std::string& fragment, std::string* err);

std::vector<PlayerInfo> ListObservedPlayers();

}  // namespace readyup

