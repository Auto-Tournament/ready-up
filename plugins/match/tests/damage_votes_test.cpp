// Offline tests for the end-of-round damage report bookkeeping (readyup/damage_ledger.h), the
// .gg / .stop vote logic (readyup/vote_logic.h) and their rules (readyup/match_rules.h).
// ctest `match_damage_votes`.
#include "readyup/damage_ledger.h"
#include "readyup/match_rules.h"
#include "readyup/vote_logic.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace readyup;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static const char* kDash = " \xE2\x80\x94 ";

static void TestLedger() {
  DamageLedger l;
  // Slot 1 (CT) hits slot 5 (T) twice, then kills it with an overkill AWP shot.
  l.OnHurt(1, 5, 27, 73);
  l.OnHurt(1, 5, 27, 46);
  CHECK(l.Dealt(1, 5).dmg == 54 && l.Dealt(1, 5).hits == 2);
  CHECK(l.Health(5) == 46);
  l.OnHurt(1, 5, 448, 0);  // capped at the 46 hp left
  CHECK(l.Dealt(1, 5).dmg == 100 && l.Dealt(1, 5).hits == 3);
  l.OnDeath(5);
  CHECK(l.Health(5) == 0);
  // Nothing the other way.
  CHECK(l.Dealt(5, 1).dmg == 0 && l.Dealt(5, 1).hits == 0);
  CHECK(l.Health(1) == 100);
  // World / fall damage and self damage move health only.
  l.OnHurt(-1, 2, 10, 90);
  l.OnHurt(2, 2, 15, 75);
  CHECK(l.Health(2) == 75 && l.Dealt(2, 2).dmg == 0 && !l.Dealt(-1, 2).hits);
  // A hit on a player with 0 hp left (dead) counts nothing more.
  l.OnHurt(3, 5, 20, 0);
  CHECK(l.Dealt(3, 5).dmg == 0 && l.Dealt(3, 5).hits == 0);
  // Zero-damage events (armor only) are not hits.
  l.OnHurt(6, 1, 0, 100);
  CHECK(l.Dealt(6, 1).hits == 0);
  CHECK(l.AnyPlayerDamage());
  // New round.
  l.Reset();
  CHECK(l.Health(5) == 100 && l.Dealt(1, 5).dmg == 0 && !l.AnyPlayerDamage());
}

static void TestReport() {
  DamageLedger l;
  l.OnHurt(1, 5, 30, 70);   // me -> Bob
  l.OnHurt(5, 1, 25, 75);   // Bob -> me
  l.OnHurt(6, 1, 100, 0);   // Eve kills me (75 left)
  l.OnDeath(1);
  l.OnHurt(2, 1, 5, 0);     // team damage on a dead player: not in the report either way
  const std::vector<DamageParticipant> players = {
      {1, "Me", 3}, {2, "Mate", 3}, {5, "Bob", 2}, {6, "Eve", 2}, {9, "Spec", 1}};
  const auto lines = FormatDamageReport(players[0], players, l);
  CHECK(lines.size() == 2);
  if (lines.size() == 2) {
    CHECK(lines[0] == std::string("To: 30 in 1 | From: 25 in 1") + kDash + "Bob (70 hp)");
    CHECK(lines[1] == std::string("To: 0 in 0 | From: 75 in 1") + kDash + "Eve (100 hp)");
  }
  // Bob's view: one opponent line per CT player.
  const auto bob = FormatDamageReport(players[2], players, l);
  CHECK(bob.size() == 2);
  if (bob.size() == 2) {
    CHECK(bob[0] == std::string("To: 25 in 1 | From: 30 in 1") + kDash + "Me (0 hp)");
    CHECK(bob[1] == std::string("To: 0 in 0 | From: 0 in 0") + kDash + "Mate (100 hp)");
  }
  // Spectators get nothing.
  CHECK(FormatDamageReport(players[4], players, l).empty());
  // No opponents: nothing.
  CHECK(FormatDamageReport(players[0], {players[0], players[1]}, l).empty());
}

static void TestGgMath() {
  CHECK(GgVotesNeeded(5, 80) == 4);
  CHECK(GgVotesNeeded(4, 80) == 4);   // 3.2 -> 4
  CHECK(GgVotesNeeded(2, 80) == 2);
  CHECK(GgVotesNeeded(1, 80) == 1);
  CHECK(GgVotesNeeded(5, 50) == 3);
  CHECK(GgVotesNeeded(5, 100) == 5);
  CHECK(GgVotesNeeded(5, 1) == 1);
  CHECK(GgVotesNeeded(0, 80) == 0);
  CHECK(GgVotesNeeded(3, 0) == 1);    // clamped to 1%
  CHECK(GgVotesNeeded(3, 250) == 3);  // clamped to 100%

  CHECK(GgScoreAllowed(2, 10, 8));
  CHECK(!GgScoreAllowed(3, 10, 8));
  CHECK(!GgScoreAllowed(10, 2, 8));   // the winning team cannot surrender early
  CHECK(GgScoreAllowed(5, 5, 0));     // 0 = any score
  CHECK(GgScoreAllowed(6, 5, 0));
  CHECK(GgScoreAllowed(0, 0, -3));    // negative treated as 0
}

static void TestGgVote() {
  GgVote v;
  // 5 players at 80%: 4 votes.
  CHECK(v.Add(11, 100.0, 5, 80, 60) == GgVote::Result::Counted);
  CHECK(v.Running() && v.Count() == 1 && v.Deadline() == 160.0);
  CHECK(v.Add(11, 101.0, 5, 80, 60) == GgVote::Result::AlreadyVoted);
  CHECK(v.Add(12, 102.0, 5, 80, 60) == GgVote::Result::Counted);
  CHECK(v.Add(13, 103.0, 5, 80, 60) == GgVote::Result::Counted);
  CHECK(v.Add(14, 104.0, 5, 80, 60) == GgVote::Result::Passed);
  CHECK(!v.Running());  // a passed vote is finished

  // Expiry: votes older than the window start over.
  CHECK(v.Add(11, 200.0, 5, 80, 60) == GgVote::Result::Counted);
  CHECK(!v.Expire(259.0));
  CHECK(v.Expire(260.0));
  CHECK(!v.Running());
  CHECK(!v.Expire(261.0));  // reported once
  CHECK(v.Add(12, 300.0, 5, 80, 60) == GgVote::Result::Counted && v.Count() == 1);
  // A late vote after the deadline starts a new vote instead of completing the old one.
  CHECK(v.Add(13, 400.0, 5, 80, 60) == GgVote::Result::Counted && v.Count() == 1);

  // A player who leaves lowers the bar: 4 eligible at 80% = 4, 3 eligible = 3.
  GgVote w;
  CHECK(w.Add(1, 0, 4, 80, 60) == GgVote::Result::Counted);
  CHECK(w.Add(2, 1, 4, 80, 60) == GgVote::Result::Counted);
  CHECK(w.Add(3, 2, 3, 80, 60) == GgVote::Result::Passed);

  // Solo team (1 player): the first vote passes.
  GgVote solo;
  CHECK(solo.Add(7, 0, 1, 80, 60) == GgVote::Result::Passed);
}

static void TestStopVote() {
  StopVote s;
  CHECK(s.Pending() == 0);
  CHECK(s.Request(1, 10.0, 30) == StopVote::Result::Waiting);
  CHECK(s.Pending() == 1);
  CHECK(s.Request(1, 11.0, 30) == StopVote::Result::AlreadyRequested);
  CHECK(s.Request(2, 20.0, 30) == StopVote::Result::Passed);
  CHECK(s.Pending() == 0);

  // The other team too late: the request expired, theirs is a new request.
  CHECK(s.Request(2, 100.0, 30) == StopVote::Result::Waiting);
  int team = 0;
  CHECK(!s.Expire(129.0, &team));
  CHECK(s.Expire(130.0, &team) && team == 2);
  CHECK(s.Pending() == 0);
  CHECK(s.Request(1, 131.0, 30) == StopVote::Result::Waiting && s.Pending() == 1);
  // Request() expires stale requests itself (no tick in between).
  CHECK(s.Request(2, 200.0, 30) == StopVote::Result::Waiting && s.Pending() == 2);
  // Reset (new round) drops it.
  s.Reset();
  CHECK(s.Pending() == 0);
  CHECK(s.Request(0, 1.0, 30) == StopVote::Result::Waiting && s.Pending() == 0);  // not a team
}

static void TestRules() {
  const MatchRules d = BuiltinDefaultRules();
  CHECK(d.gg_enabled == 0 && d.gg_threshold_pct == 80 && d.gg_min_score_diff == 8);
  CHECK(d.stop_command_available == 0 && d.stop_command_no_damage == 0 && d.stop_vote_seconds == 30);
  MatchRules cfg;
  cfg.gg_enabled = 1;
  cfg.gg_min_score_diff = 5;
  cfg.stop_vote_seconds = 1;  // clamped to 5
  MatchRules match;
  match.gg_threshold_pct = 60;
  match.gg_min_score_diff = 0;
  match.stop_command_available = 1;
  const MatchRules r = ResolveRules(match, cfg);
  CHECK(r.gg_enabled == 1 && r.gg_threshold_pct == 60 && r.gg_min_score_diff == 0);
  CHECK(r.stop_command_available == 1 && r.stop_command_no_damage == 0 && r.stop_vote_seconds == 5);

  CHECK(GgThresholdPctFromText("0.8") == 80);
  CHECK(GgThresholdPctFromText("0.75") == 75);
  CHECK(GgThresholdPctFromText("80") == 80);
  CHECK(GgThresholdPctFromText("1") == 100);
  CHECK(GgThresholdPctFromText("0.001") == 1);
  CHECK(GgThresholdPctFromText("0") == -1);
  CHECK(GgThresholdPctFromText("150") == -1);
  CHECK(GgThresholdPctFromText("abc") == -1);
  CHECK(GgThresholdPctFromText("") == -1);
}

int main() {
  TestLedger();
  TestReport();
  TestGgMath();
  TestGgVote();
  TestStopVote();
  TestRules();
  std::printf("match_damage_votes: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
