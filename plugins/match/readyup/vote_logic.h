#pragma once

// The `.gg` surrender vote and the `.stop` round-restore vote: pure logic, no engine calls
// (ctest `match_damage_votes`). The engine side is votes.h.

#include <cstdint>
#include <set>

namespace readyup {

// ---- .gg ---------------------------------------------------------------------------------------

// Yes votes a team of `eligible` connected players needs at `thresholdPct` percent (1..100):
// ceil(eligible * pct / 100), at least 1. 0 when nobody is eligible.
int GgVotesNeeded(int eligible, int thresholdPct);
// A team may surrender while it trails by at least `minScoreDiff` rounds (0 = any score).
bool GgScoreAllowed(int teamScore, int otherScore, int minScoreDiff);

// One team's surrender vote. Votes expire `windowSeconds` after the first one.
class GgVote {
 public:
  enum class Result { Counted, AlreadyVoted, Passed };
  // voter: a player id; now: seconds (monotonic).
  Result Add(uint64_t voter, double now, int eligible, int thresholdPct, double windowSeconds);
  // Expired or finished votes are dropped. True if a running vote timed out just now.
  bool Expire(double now);
  void Reset();
  bool Running() const { return !voters_.empty(); }
  int Count() const { return static_cast<int>(voters_.size()); }
  double Deadline() const { return deadline_; }

 private:
  std::set<uint64_t> voters_;
  double deadline_ = 0;
};

// ---- .stop -------------------------------------------------------------------------------------

// Both teams ask for a restore of the current round within `windowSeconds` of each other.
class StopVote {
 public:
  enum class Result { Waiting, AlreadyRequested, Passed };
  // team: 1 / 2.
  Result Request(int team, double now, double windowSeconds);
  // True if a pending request timed out just now (the team that asked: *team).
  bool Expire(double now, int* team);
  void Reset();
  // The team whose request is pending (0 = none).
  int Pending() const;

 private:
  double at_[3] = {0, 0, 0};  // request time per team (0 = none)
  double window_ = 0;
};

}  // namespace readyup
