#include "readyup/vote_logic.h"

#include <algorithm>

namespace readyup {

int GgVotesNeeded(int eligible, int thresholdPct) {
  if (eligible <= 0) return 0;
  const int pct = std::clamp(thresholdPct, 1, 100);
  const int need = (eligible * pct + 99) / 100;
  return std::clamp(need, 1, eligible);
}

bool GgScoreAllowed(int teamScore, int otherScore, int minScoreDiff) {
  return minScoreDiff <= 0 || otherScore - teamScore >= minScoreDiff;
}

GgVote::Result GgVote::Add(uint64_t voter, double now, int eligible, int thresholdPct, double windowSeconds) {
  (void)Expire(now);
  if (voters_.empty()) deadline_ = now + windowSeconds;
  if (!voters_.insert(voter).second) return Result::AlreadyVoted;
  const int need = GgVotesNeeded(eligible, thresholdPct);
  if (need > 0 && Count() >= need) {
    Reset();
    return Result::Passed;
  }
  return Result::Counted;
}

bool GgVote::Expire(double now) {
  if (voters_.empty() || now < deadline_) return false;
  Reset();
  return true;
}

void GgVote::Reset() {
  voters_.clear();
  deadline_ = 0;
}

StopVote::Result StopVote::Request(int team, double now, double windowSeconds) {
  if (team != 1 && team != 2) return Result::Waiting;
  int dummy = 0;
  (void)Expire(now, &dummy);
  const int other = team == 1 ? 2 : 1;
  if (at_[other] > 0) {
    Reset();
    return Result::Passed;
  }
  if (at_[team] > 0) return Result::AlreadyRequested;
  at_[team] = now > 0 ? now : 1e-9;
  window_ = windowSeconds;
  return Result::Waiting;
}

bool StopVote::Expire(double now, int* team) {
  for (int t = 1; t <= 2; ++t) {
    if (at_[t] > 0 && now - at_[t] >= window_) {
      at_[t] = 0;
      if (team) *team = t;
      return true;
    }
  }
  return false;
}

void StopVote::Reset() {
  at_[1] = at_[2] = 0;
}

int StopVote::Pending() const { return at_[1] > 0 ? 1 : at_[2] > 0 ? 2 : 0; }

}  // namespace readyup
