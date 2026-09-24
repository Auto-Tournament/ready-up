// Offline tests for the match stats model (match_stats.h), the map/series-end logic
// (match_end.h) and demo naming (demo_recorder.h). No CS2 server needed:
//   cmake --build build && (cd build && ctest --output-on-failure)

#include "readyup/demo_recorder.h"
#include "readyup/match_end.h"
#include "readyup/match_stats.h"
#include "readyup/minijson.h"

#include <cstdio>
#include <string>

using namespace readyup;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

#define CHECK_EQ(a, b)                                                                            \
  do {                                                                                            \
    const auto va = (a);                                                                          \
    const auto vb = (b);                                                                          \
    if (!(va == vb)) {                                                                            \
      std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                   static_cast<long long>(va), static_cast<long long>(vb));                       \
      ++g_failures;                                                                               \
    }                                                                                             \
  } while (0)

static void CheckStr(const std::string& a, const std::string& b, const char* expr, int line) {
  if (a == b) return;
  std::fprintf(stderr, "%s:%d: CHECK_STR failed: %s: \"%s\" != \"%s\"\n", __FILE__, line, expr, a.c_str(),
               b.c_str());
  ++g_failures;
}
#define CHECK_STR(a, b) CheckStr((a), (b), #a, __LINE__)

static const stats::PlayerLine* FindPlayer(const stats::MapStats& m, uint64_t id) {
  for (const auto& p : m.players) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

static bool ParsesAsJson(const std::string& s) {
  minijson::ParseError err;
  auto v = minijson::Parse(s, &err);
  if (!v) std::fprintf(stderr, "JSON parse error at %zu: %s\n", err.offset, err.msg.c_str());
  return v.has_value();
}

// team1 = A1 (1), A2 (2) starting CT; team2 = B1 (11), B2 (12) starting T.
constexpr uint64_t A1 = 76561198000000001ull, A2 = 76561198000000002ull;
constexpr uint64_t B1 = 76561198000000011ull, B2 = 76561198000000012ull;

static void ObserveAll(stats::StatsAccumulator& st, int team1Side) {
  const int team2Side = team1Side == 3 ? 2 : 3;
  st.ObservePlayer(A1, "A1", team1Side, 1);
  st.ObservePlayer(A2, "A2", team1Side, 1);
  st.ObservePlayer(B1, "B1", team2Side, 2);
  st.ObservePlayer(B2, "B2", team2Side, 2);
}

static void TestStatsRound1() {
  stats::StatsAccumulator st;
  st.BeginMap(/*team1IsCt=*/true);
  st.OnRoundStart();
  ObserveAll(st, 3);

  st.OnPlayerHurt(B1, A1, 150, 0, "ak47");       // overkill: counts 100
  st.OnPlayerDeath(B1, A1, 0, false, true, "ak47", 10.0);
  st.OnPlayerBlind(A1, B2, 2.0);                 // enemy flashed
  st.OnPlayerBlind(B1, B2, 1.0);                 // friendly flashed
  st.OnPlayerBlind(A2, B2, 0.0);                 // zero duration: ignored
  st.OnPlayerHurt(A2, B2, 100, 0, "glock");
  st.OnPlayerDeath(A2, B2, 0, false, false, "glock", 12.0);
  // A1 is now alone (1v1) and trades A2 within 5 s.
  st.OnPlayerHurt(B2, A1, 100, 0, "ak47");
  st.OnPlayerDeath(B2, A1, 0, false, false, "ak47", 14.0);
  st.OnRoundMvp(A1);
  st.SetScore(A1, 7);
  st.OnRoundEnd(3, 8);

  const auto m = st.Snapshot();
  CHECK_EQ(m.team1.score, 1);
  CHECK_EQ(m.team1.score_ct, 1);
  CHECK_EQ(m.team2.score, 0);
  CHECK_EQ(m.rounds.size(), 1u);
  CHECK_EQ(m.rounds[0].winner_team, 1);
  CHECK_EQ(m.rounds[0].winner_side, 3);
  CHECK_EQ(m.rounds[0].reason, 8);

  const auto* a1 = FindPlayer(m, A1);
  const auto* a2 = FindPlayer(m, A2);
  const auto* b1 = FindPlayer(m, B1);
  const auto* b2 = FindPlayer(m, B2);
  CHECK(a1 && a2 && b1 && b2);
  if (!(a1 && a2 && b1 && b2)) return;
  CHECK_EQ(a1->stats.kills, 2);
  CHECK_EQ(a1->stats.headshot_kills, 1);
  CHECK_EQ(a1->stats.damage, 200);
  CHECK_EQ(a1->stats.entry_kills_ct, 1);
  CHECK_EQ(a1->stats.trade_kills, 1);
  CHECK_EQ(a1->stats.multi_kills[1], 1);
  CHECK_EQ(a1->stats.clutches_won[0], 1);
  CHECK_EQ(a1->stats.kast_rounds, 1);
  CHECK_EQ(a1->stats.rounds_played, 1);
  CHECK_EQ(a1->stats.mvp, 1);
  CHECK_EQ(a1->stats.score, 7);
  CHECK_EQ(a2->stats.deaths, 1);
  CHECK_EQ(a2->stats.traded_deaths, 1);
  CHECK_EQ(a2->stats.kast_rounds, 1);  // traded
  CHECK_EQ(b1->stats.entry_deaths_t, 1);
  CHECK_EQ(b1->stats.kast_rounds, 0);
  CHECK_EQ(b2->stats.kills, 1);
  CHECK_EQ(b2->stats.enemies_flashed, 1);
  CHECK_EQ(b2->stats.friendlies_flashed, 1);
  CHECK_EQ(b2->stats.kast_rounds, 1);
  CHECK_EQ(b2->stats.clutches_won[1], 0);  // was last alive vs 2 but lost

  // Round 2: team damage, utility damage, flash assist, suicide, team kill.
  st.OnRoundStart();
  ObserveAll(st, 3);
  st.OnPlayerHurt(B1, B2, 50, 50, "ak47");           // team damage: not counted
  st.OnPlayerHurt(A1, B1, 40, 60, "hegrenade");
  st.OnPlayerDeath(A1, B1, B2, true, false, "ak47", 30.0);
  st.OnPlayerDeath(A2, 0, 0, false, false, "world", 31.0);
  st.OnPlayerDeath(B2, B1, 0, false, false, "ak47", 32.0);  // team kill
  st.OnRoundEnd(2, 9);

  const auto m2 = st.Snapshot();
  CHECK_EQ(m2.team2.score, 1);
  CHECK_EQ(m2.team2.score_t, 1);
  CHECK_EQ(m2.rounds.size(), 2u);
  const auto* b1r = FindPlayer(m2, B1);
  const auto* b2r = FindPlayer(m2, B2);
  const auto* a2r = FindPlayer(m2, A2);
  CHECK(b1r && b2r && a2r);
  if (!(b1r && b2r && a2r)) return;
  CHECK_EQ(b1r->stats.kills, 1);
  CHECK_EQ(b1r->stats.team_kills, 1);
  CHECK_EQ(b1r->stats.damage, 40);
  CHECK_EQ(b1r->stats.utility_damage, 40);
  CHECK_EQ(b2r->stats.flash_assists, 1);
  CHECK_EQ(b2r->stats.assists, 0);
  CHECK_EQ(b2r->stats.damage, 100);  // round 1 only; B2 hitting B1 in round 2 is team damage
  CHECK_EQ(a2r->stats.suicides, 1);
  CHECK_EQ(a2r->stats.kills, 0);
  CHECK_EQ(a2r->stats.rounds_played, 2);

  const std::string json = stats::ToJson(m2);
  CHECK(ParsesAsJson(json));
  CHECK(json.find("\"clutches_won\":[1,0,0,0,0]") != std::string::npos);
  CHECK(json.find("\"id\":\"76561198000000001\"") != std::string::npos);
}

static void TestStatsSidesAndGate() {
  stats::StatsAccumulator st;
  // Not live: nothing is recorded.
  st.ObservePlayer(A1, "A1", 3, 1);
  st.OnPlayerDeath(B1, A1, 0, false, false, "ak47", 1.0);
  st.OnRoundEnd(3, 8);
  CHECK_EQ(st.Snapshot().rounds.size(), 0u);
  CHECK_EQ(st.Team1Score(), 0);

  // team2 starts CT; after the swap team1 is CT.
  st.BeginMap(/*team1IsCt=*/false);
  st.OnRoundStart();
  st.OnRoundEnd(3, 7);
  CHECK_EQ(st.Team2Score(), 1);
  st.SetTeam1IsCt(true);
  st.OnRoundStart();
  st.OnRoundEnd(3, 7);
  CHECK_EQ(st.Team1Score(), 1);
  const auto m = st.Snapshot();
  CHECK_EQ(m.team1.score_ct, 1);
  CHECK_EQ(m.team2.score_ct, 1);
  // A player without a roster team gets it from the side it was first seen on.
  st.OnRoundStart();
  st.ObservePlayer(999, "bot", 2, 0, /*bot=*/true);
  const auto m2 = st.Snapshot();
  const auto* bot = FindPlayer(m2, 999);
  CHECK(bot && bot->team == 2 && bot->bot);
  st.EndMap();
  CHECK(!st.Live());
}

static void TestMapEndPlan() {
  auto p = ComputeMapEndPlan(/*demo=*/false, /*upload=*/false, 10, 5, 10, 60);
  CHECK_EQ(p.restartDelay, 10);
  CHECK_EQ(p.tvFlushDelay, 0);
  CHECK_EQ(p.kickDelay, 14);
  p = ComputeMapEndPlan(true, false, 0, 5, 10, 60);
  CHECK_EQ(p.tvFlushDelay, 15);
  CHECK_EQ(p.restartDelay, 15);
  CHECK_EQ(p.kickDelay, 24);
  p = ComputeMapEndPlan(true, true, 10, 5, 10, 60);
  CHECK_EQ(p.tvFlushDelay, 25);
  CHECK_EQ(p.restartDelay, 35);
  CHECK_EQ(p.kickDelay, 94);
  p = ComputeMapEndPlan(true, true, 120, 5, 10, 60);
  CHECK_EQ(p.tvFlushDelay, 135);
}

static void TestSeriesOver() {
  // Bo1.
  CHECK(IsSeriesOver(1, RemainingMaps(1, 1, 1), 1, 0, true));
  // Bo3 after map 1 (1-0): continue.
  CHECK_EQ(RemainingMaps(3, 3, 1), 2);
  CHECK(!IsSeriesOver(3, RemainingMaps(3, 3, 1), 1, 0, true));
  // Bo3 2-0 after map 2: clinched; played out without clinch.
  CHECK(IsSeriesOver(3, RemainingMaps(3, 3, 2), 2, 0, true));
  CHECK(!IsSeriesOver(3, RemainingMaps(3, 3, 2), 2, 0, false));
  // Bo3 1-1 with a drawn map: no maps left after map 3.
  CHECK(IsSeriesOver(3, RemainingMaps(3, 3, 3), 1, 1, true));
  // Map list shorter than num_maps.
  CHECK_EQ(RemainingMaps(5, 2, 2), 0);
  // Bo2 1-1: over after map 2 (a draw).
  CHECK(IsSeriesOver(2, RemainingMaps(2, 2, 2), 1, 1, true));
}

static void TestDemoNaming() {
  demo::TokenValues v;
  v.time = "2026-09-25_12-00-00";
  v.matchid = 42;
  v.slug = "r1m1";
  v.map = "de_dust2";
  v.mapNumber = 2;
  v.team1 = "Team A";
  v.team2 = "B/C";
  v.team1Score = 13;
  v.team2Score = 11;
  CHECK_STR(demo::FormatDemoFileName("{TIME}_{MATCH_ID}_{MAP}_{TEAM1}_vs_{TEAM2}", v), std::string("2026-09-25_12-00-00_42_de_dust2_Team_A_vs_B_C"));
  CHECK_STR(demo::ExpandTokens("m{MAP_NUMBER}/i{MAP_INDEX}/{TEAM1_SCORE}-{TEAM2_SCORE}/{SLUG}", v), std::string("m2/i1/13-11/r1m1"));
  v.fileName = "x.dem";
  v.roundNumber = 24;
  CHECK_STR(demo::ExpandTokens("{FILENAME}:{ROUND_NUMBER}", v), std::string("x.dem:24"));
  CHECK_STR(demo::FormatDemoFileName("../{MAP}", v), std::string("_de_dust2"));

  std::vector<demo::DemoCandidate> files = {
      {"/csgo/ReadyUp/old_42_de_dust2.dem", 100},
      {"/csgo/ReadyUp/new_42_de_dust2.dem", 200},
      {"/csgo/ReadyUp/new_420_de_dust2.dem", 300},
      {"/csgo/ReadyUp/new_42_de_mirage.dem", 400},
  };
  CHECK_STR(demo::PickDemoForMatch(files, "missing.dem", 42, "de_dust2", 0), std::string("/csgo/ReadyUp/new_42_de_dust2.dem"));
  CHECK_STR(demo::PickDemoForMatch(files, "old_42_de_dust2.dem", 42, "de_dust2", 0), std::string("/csgo/ReadyUp/old_42_de_dust2.dem"));
  CHECK_STR(demo::PickDemoForMatch(files, "missing.dem", 42, "de_dust2", 250), std::string());

  demo::DemoEvent e;
  e.type = demo::DemoEventType::UploadFailed;
  e.fileName = "a\"b.dem";
  e.error = "HTTP 500";
  CHECK(ParsesAsJson(demo::ToJson(e)));
}

static void TestMatchFlowEventJson() {
  stats::StatsAccumulator st;
  st.BeginMap(true);
  st.OnRoundStart();
  ObserveAll(st, 3);
  st.OnPlayerDeath(B1, A1, 0, false, false, "ak47", 1.0);
  st.OnRoundEnd(3, 8);
  MatchFlowEvent e;
  e.type = MatchFlowEventType::MapResult;
  e.matchid = 7;
  e.slug = "s\"lug";
  e.winner = "team1";
  e.team1Score = 13;
  e.team2Score = 5;
  e.team1SeriesScore = 1;
  e.stats = st.Snapshot();
  const std::string j = ToJson(e);
  CHECK(ParsesAsJson(j));
  CHECK(j.find("\"type\":\"map_result\"") != std::string::npos);
  e.type = MatchFlowEventType::SeriesEnd;
  e.secondsUntilReset = 94;
  const std::string j2 = ToJson(e);
  CHECK(ParsesAsJson(j2));
  CHECK(j2.find("\"seconds_until_reset\":94") != std::string::npos);
}

int main() {
  TestStatsRound1();
  TestStatsSidesAndGate();
  TestMapEndPlan();
  TestSeriesOver();
  TestDemoNaming();
  TestMatchFlowEventJson();
  if (g_failures) {
    std::fprintf(stderr, "match_flow_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("match_flow_test: OK\n");
  return 0;
}
