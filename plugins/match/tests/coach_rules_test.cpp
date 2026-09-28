// Offline tests for the coach policy, the side a coach watches and the admin command's player
// lookup (readyup/coach_rules.h), plus the match config side: team1/team2 `coaches`,
// `coaches_per_team` (match_config_parser.cpp) and the fleet role `coach` (fleet_state.cpp).
// ctest `match_coach_rules`.
#include "readyup/coach_rules.h"
#include "readyup/fleet_state.h"
#include "readyup/match_config_parser.h"
#include "readyup/status_snapshot.h"

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

static void TestParseArg() {
  CHECK(ParseCoachArg("") == CoachArg::None);
  CHECK(ParseCoachArg("ct") == CoachArg::Ct && ParseCoachArg("CT") == CoachArg::Ct);
  CHECK(ParseCoachArg("t") == CoachArg::T && ParseCoachArg("T") == CoachArg::T);
  CHECK(ParseCoachArg("team1") == CoachArg::Team1 && ParseCoachArg("Team2") == CoachArg::Team2);
  CHECK(ParseCoachArg("terrorist") == CoachArg::Invalid && ParseCoachArg("3") == CoachArg::Invalid);
}

static CoachRequest Listed(int team) {
  CoachRequest r;
  r.coaching_possible = true;
  r.listed = true;
  r.listed_team = team;
  return r;
}

static void TestDecide() {
  // A coach listed for team2 types `.coach`: coaches team2.
  {
    CoachDecision d = DecideCoach(Listed(2), "Alpha", "Bravo");
    CHECK(d.ok && d.team == 2 && !d.already);
  }
  // ... `.coach ct` while team2 is on CT: fine; `.coach ct` while team1 is CT: refused (their team).
  {
    CoachRequest r = Listed(2);
    r.arg = CoachArg::Ct;
    r.team1_is_ct = false;
    CHECK(DecideCoach(r).ok && DecideCoach(r).team == 2);
    r.team1_is_ct = true;
    CoachDecision d = DecideCoach(r, "Alpha", "Bravo");
    CHECK(!d.ok && d.reason == "you are listed as Bravo's coach");
    // An admin may move them.
    r.by_admin = true;
    CHECK(DecideCoach(r).ok && DecideCoach(r).team == 1);
  }
  // Listed without a team (MAT top-level "coaches"): must name the side.
  {
    CoachRequest r = Listed(0);
    CoachDecision d = DecideCoach(r);
    CHECK(!d.ok && d.reason == "usage: .coach ct|t");
    r.arg = CoachArg::T;  // team1 is CT: T = team2
    CHECK(DecideCoach(r).ok && DecideCoach(r).team == 2);
    r.arg = CoachArg::Team1;
    CHECK(DecideCoach(r).ok && DecideCoach(r).team == 1);
  }
  // Invalid argument.
  {
    CoachRequest r = Listed(1);
    r.arg = CoachArg::Invalid;
    CHECK(!DecideCoach(r).ok && DecideCoach(r).reason == "usage: .coach ct|t");
    r.by_admin = true;
    CHECK(DecideCoach(r).reason == "usage: ru match coach <player> team1|team2|ct|t");
  }
  // Rostered players never coach, not even by an admin.
  {
    CoachRequest r = Listed(1);
    r.roster_team = 1;
    CoachDecision d = DecideCoach(r, "Alpha", "Bravo");
    CHECK(!d.ok && d.reason == "you are on Alpha's roster: players cannot coach");
    r.by_admin = true;
    CHECK(!DecideCoach(r).ok && DecideCoach(r, "Alpha").reason == "they are on Alpha's roster: players cannot coach");
  }
  // Not listed, a real match: only an admin can make them a coach.
  {
    CoachRequest r;
    r.coaching_possible = true;
    r.arg = CoachArg::Ct;
    CoachDecision d = DecideCoach(r);
    CHECK(!d.ok && d.reason.find("only the coaches in the match config") == 0);
    r.by_admin = true;
    CHECK(DecideCoach(r).ok && DecideCoach(r).team == 1);
  }
  // Scrims are open.
  {
    CoachRequest r;
    r.coaching_possible = true;
    r.scrim = true;
    r.arg = CoachArg::T;
    CHECK(DecideCoach(r).ok && DecideCoach(r).team == 2);
    // ... up to per_team_limit unlisted coaches per team.
    r.coaches_team2 = 2;
    CoachDecision d = DecideCoach(r);
    CHECK(!d.ok && d.reason == "team2 already has 2 coaches");
    r.per_team_limit = 0;  // no limit
    CHECK(DecideCoach(r).ok);
    r.per_team_limit = 1;
    r.coaches_team2 = 1;
    CHECK(DecideCoach(r).reason == "team2 already has 1 coach");
    r.arg = CoachArg::Ct;  // the other team has room
    CHECK(DecideCoach(r).ok && DecideCoach(r).team == 1);
  }
  // Listed coaches are never limited.
  {
    CoachRequest r = Listed(1);
    r.coaches_team1 = 5;
    r.per_team_limit = 1;
    CHECK(DecideCoach(r).ok);
  }
  // Already coaching that team.
  {
    CoachRequest r = Listed(1);
    r.current_team = 1;
    CoachDecision d = DecideCoach(r);
    CHECK(d.ok && d.already && d.team == 1);
  }
  // Switching teams (scrim): allowed, the old team is left.
  {
    CoachRequest r;
    r.coaching_possible = true;
    r.scrim = true;
    r.current_team = 1;
    r.arg = CoachArg::Team2;
    CoachDecision d = DecideCoach(r);
    CHECK(d.ok && !d.already && d.team == 2);
  }
  // No match / scrim, or the ruleset keeps coaches out (valve online).
  {
    CoachRequest r = Listed(1);
    r.coaching_possible = false;
    CHECK(!DecideCoach(r).ok && DecideCoach(r).reason.find("coaching needs a match") == 0);
    r = Listed(1);
    r.admitted = false;
    CHECK(!DecideCoach(r).ok && DecideCoach(r).reason.find("coaches are not admitted") == 0);
    r.by_admin = true;
    CHECK(!DecideCoach(r).ok);
  }
  CHECK(CoachTeamLabel(1, "", "") == "team1" && CoachTeamLabel(2, "A", "B") == "B" && CoachTeamLabel(0, "A", "B") == "no team");
}

static void TestSides() {
  SideTally t;
  // Nobody on a side: the fallback, or the engine's current value.
  CHECK(Team1IsCtFromTally(t, true) && !Team1IsCtFromTally(t, false));
  CHECK(CoachingSide(1, t, 0, true) == 3 && CoachingSide(2, t, 0, true) == 2);
  CHECK(CoachingSide(1, t, 2, true) == 2);  // the engine swapped the coach: keep it
  CHECK(CoachingSide(0, t, 3, true) == 0);
  // team1 on T (after a knife .switch, or the second half).
  t.team1_t = 5;
  CHECK(!Team1IsCtFromTally(t, true));
  CHECK(CoachingSide(1, t, 3, true) == 2 && CoachingSide(2, t, 0, true) == 3);
  // Mid-swap tie on team1: team2 decides.
  t = SideTally{};
  t.team1_ct = 2;
  t.team1_t = 2;
  t.team2_ct = 1;
  t.team2_t = 4;
  CHECK(Team1IsCtFromTally(t, false));
  CHECK(CoachingSide(2, t, 3, false) == 2);
  // Only team2 connected, on CT: team1 is T.
  t = SideTally{};
  t.team2_ct = 3;
  CHECK(CoachingSide(1, t, 0, true) == 2);
  // Everything tied: undecided -> the current value wins over the fallback.
  t = SideTally{};
  t.team1_ct = t.team1_t = 1;
  t.team2_ct = t.team2_t = 1;
  CHECK(CoachingSide(1, t, 2, true) == 2 && CoachingSide(1, t, 0, true) == 3);
}

static void TestResolvePlayer() {
  const std::vector<CoachCandidate> ps = {
      {76561198000000001ull, 3, "Coach Carter"},
      {76561198000000002ull, 4, "carter"},
      {76561198000000003ull, 7, "Zywoo"},
      {76561198000000004ull, 8, "123"},
  };
  std::string err;
  CHECK(ResolveCoachPlayer("76561198000000003", ps, &err) == 76561198000000003ull);
  CHECK(ResolveCoachPlayer("[U:1:39734275]", ps, &err) == 76561198000000003ull);  // Steam3 of the same account
  CHECK(ResolveCoachPlayer("[U:1:1]", ps, &err) == 0 && err.find("is not connected") != std::string::npos);
  CHECK(ResolveCoachPlayer("#7", ps, &err) == 76561198000000003ull);
  CHECK(ResolveCoachPlayer("#9", ps, &err) == 0 && err == "no connected player with userid #9");
  CHECK(ResolveCoachPlayer("zyw", ps, &err) == 76561198000000003ull);
  CHECK(ResolveCoachPlayer("CARTER", ps, &err) == 76561198000000002ull);  // exact match first
  CHECK(ResolveCoachPlayer("arte", ps, &err) == 0 && err.find("2 players match") == 0);
  CHECK(ResolveCoachPlayer("123", ps, &err) == 76561198000000004ull);  // a short number is a name
  CHECK(ResolveCoachPlayer("nobody", ps, &err) == 0 && err.find("no connected player matches") == 0);
  CHECK(ResolveCoachPlayer("", ps, &err) == 0);
}

static void TestMatchConfig() {
  // get5 / MAT: per-team coaches as {steamid64: name} or [steamid64]; coaches_per_team.
  const std::string json = R"({
    "matchid": 7, "num_maps": 1, "maplist": ["de_mirage"], "map_sides": ["team1_ct"],
    "coaches_per_team": 1,
    "team1": {"name": "Alpha", "players": {"76561198000000011": "a1"},
              "coaches": {"76561198000000021": "coachA"}},
    "team2": {"name": "Bravo", "players": {"76561198000000012": "b1"},
              "coaches": ["76561198000000022", "76561198000000012"]},
    "coaches": ["76561198000000023"]
  })";
  std::string err;
  auto ctx = ParseWebhookMatchContextFromJson(json, &err);
  CHECK(ctx.has_value());
  if (ctx) {
    CHECK(ctx->coaches_per_team == 1);
    CHECK(ctx->coach_team.size() == 2);
    CHECK(ctx->coach_team.count(76561198000000021ull) && ctx->coach_team.at(76561198000000021ull) == WebhookTeam::Team1);
    CHECK(ctx->coach_team.count(76561198000000022ull) && ctx->coach_team.at(76561198000000022ull) == WebhookTeam::Team2);
    CHECK(!ctx->coach_team.count(76561198000000012ull));  // a rostered player is not a coach
    CHECK(ctx->coaches.count(76561198000000021ull) && ctx->coaches.count(76561198000000022ull) &&
          ctx->coaches.count(76561198000000023ull));
    // Default ruleset: coaches are whitelisted spectators, never on the roster.
    CHECK(ctx->spectators.count(76561198000000021ull) && ctx->spectators.count(76561198000000023ull));
    CHECK(ctx->roster_team.size() == 2);
  }
  // No per-team coaches: defaults.
  auto plain = ParseWebhookMatchContextFromJson(R"({"matchid": 8, "team1": {"players": {}}, "team2": {}})", &err);
  CHECK(plain && plain->coach_team.empty() && plain->coaches_per_team == kDefaultCoachesPerTeam);

  // Fleet: role "coach" inside a team becomes that team's coach in the MAT config.
  using status::Json;
  Json cfg;
  CHECK(Json::Parse(R"({"num_maps":1,"maps":[{"number":1,"name":"de_nuke","sides":"team1_ct"}],
      "team1":{"id":"a","name":"Alpha","players":[{"steamid64":"76561198000000011","name":"a1"},
                                              {"steamid64":"76561198000000031","name":"cA","role":"coach"}]},
      "team2":{"id":"b","name":"Bravo","players":[{"steamid64":"76561198000000012","name":"b1"},
                                              {"steamid64":"76561198000000032","name":"cB","role":"coach"}]},
      "rules":{}})",
                    &cfg));
  const Json mat = fleetstate::AssignToMatConfig("m-1", cfg, nullptr);
  const Json* root = mat.Find("config");
  CHECK(root != nullptr);
  if (!root) return;
  const Json* t1 = root->Find("team1");
  CHECK(t1 && t1->Find("coaches") && t1->Find("coaches")->Find("76561198000000031"));
  CHECK(t1 && t1->Find("players") && !t1->Find("players")->Find("76561198000000031"));
  CHECK(root->Find("coaches") && root->Find("coaches")->Items().size() == 2);  // the flat list stays
  auto fctx = ParseWebhookMatchContextFromJson(mat.Dump(), &err);
  CHECK(fctx.has_value());
  if (fctx) {
    CHECK(fctx->coach_team.size() == 2);
    CHECK(fctx->coach_team.count(76561198000000031ull) && fctx->coach_team.at(76561198000000031ull) == WebhookTeam::Team1);
    CHECK(fctx->coach_team.count(76561198000000032ull) && fctx->coach_team.at(76561198000000032ull) == WebhookTeam::Team2);
    CHECK(!fctx->roster_team.count(76561198000000031ull));
  }
}

int main() {
  TestParseArg();
  TestDecide();
  TestSides();
  TestResolvePlayer();
  TestMatchConfig();
  if (g_failures) {
    std::fprintf(stderr, "coach_rules: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
  std::printf("coach_rules: %d checks passed\n", g_checks);
  return 0;
}
