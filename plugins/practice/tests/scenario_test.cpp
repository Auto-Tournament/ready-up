// Offline tests for plugins/practice/scenario.h. ctest `practice_scenario`.
#include "scenario.h"

#include <dirent.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace practice::scenario;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }

// Two T, one CT; sample every 4 ticks; 64 tick; 256 ticks long.
static const char* kScenario = R"({
  "format": "readyup.scenario", "version": 1, "id": "test_round", "title": "Test", "map": "de_nuke",
  "tickrate": 64, "sample_ticks": 4, "round": 3, "freeze_end_tick": 1000, "length": 256, "default_start": 8,
  "winner": "T", "reason": "ct_killed",
  "source": {"event": "Test Cup", "match": "A vs B", "teams": {"T": "Alpha", "CT": "Bravo"}, "url": "https://example.org/demo"},
  "players": [
    {"name": "ZywOo", "steamid": "76561198113666193", "team": "T", "alive_until": null,
     "track": [0,0,0,170,0, 40,0,0,-170,10, 80,0,0,-170,10],
     "state": [{"t": 0, "hp": 100, "armor": 100, "helmet": true, "money": 800, "items": ["weapon_knife", "weapon_ak47"]},
               {"t": 6, "hp": 73, "armor": 90, "helmet": true, "money": 800, "items": ["weapon_knife", "weapon_ak47"]}]},
    {"name": "apEX", "steamid": "76561197989744167", "team": "T", "alive_until": 6,
     "track": [10,10,10,0,0, 10,20,10,0,0],
     "state": [{"t": 0, "hp": 100, "items": ["weapon_glock", "weapon_bogus_but_prefixed", "knife"]}]},
    {"name": "zont1x", "steamid": "76561198995880877", "team": "CT", "track": [5,5,5,90,0],
     "state": [{"t": 0, "hp": 100, "defuser": true}]}
  ],
  "grenades": [
    {"t": 20, "player": 0, "type": "smoke", "pos": [1,2,3], "vel": [100,0,50], "land": [200,0,0], "land_t": 90},
    {"t": 5, "player": 2, "type": "flash", "pos": [1,2,3], "vel": [0,0,0], "land": [1,2,3], "land_t": 7}
  ],
  "deaths": [{"t": 6, "player": 1, "killer": 2, "weapon": "usp_silencer"}],
  "bomb": [{"t": 200, "event": "planted", "player": 0, "pos": [9,9,9]}]
})";

static std::string With(const std::string& from, const std::string& to) {
  std::string s = kScenario;
  const size_t at = s.find(from);
  if (at != std::string::npos) s.replace(at, from.size(), to);
  return s;
}

int main() {
  Scenario sc;
  std::string err;
  CHECK(Parse(kScenario, &sc, &err));
  if (!err.empty()) std::fprintf(stderr, "parse: %s\n", err.c_str());
  CHECK(sc.id == "test_round" && sc.map == "de_nuke" && sc.players.size() == 3);
  CHECK(sc.players[0].team == 2 && sc.players[2].team == 3 && sc.players[0].aliveUntil == -1 && sc.players[1].aliveUntil == 6);
  CHECK(sc.source.teamT == "Alpha" && sc.source.url == "https://example.org/demo");
  // Grenades are sorted by t on load.
  CHECK(sc.grenades.size() == 2 && sc.grenades[0].t == 5 && sc.grenades[0].type == GrenadeType::kFlash);
  CHECK(GrenadeDefIndex(GrenadeType::kSmoke) == 45 && GrenadeDefIndex(GrenadeType::kIncendiary) == 48);

  // Interpolation: halfway between samples; yaw wraps the short way (170 -> -170 through 180).
  Pose p;
  CHECK(sc.players[0].PoseAt(2, sc.sampleTicks, &p));
  CHECK(Near(p.pos.x, 20) && Near(std::fabs(p.yaw), 180.f));
  CHECK(sc.players[0].PoseAt(8, sc.sampleTicks, &p) && Near(p.pos.x, 80) && Near(p.pitch, 10));
  CHECK(!sc.players[0].PoseAt(9, sc.sampleTicks, &p));  // past the track
  CHECK(!sc.players[1].PoseAt(6, sc.sampleTicks, &p));  // dead at 6
  CHECK(sc.players[1].PoseAt(4, sc.sampleTicks, &p) && Near(p.pos.y, 20));
  CHECK(!sc.players[1].PoseAt(5, sc.sampleTicks, &p));  // past its last sample

  // State at a tick; items keep only weapon_* strings.
  CHECK(sc.players[0].StateAt(0).hp == 100 && sc.players[0].StateAt(5).hp == 100 && sc.players[0].StateAt(6).hp == 73);
  CHECK(sc.players[0].StateAt(0).helmet && sc.players[0].StateAt(0).money == 800);
  CHECK(sc.players[1].StateAt(0).items.size() == 2 && sc.players[2].StateAt(0).defuser);

  // Start parsing.
  int t = -1;
  CHECK(ParseStart("2", sc, &t, &err) && t == 128);
  CHECK(ParseStart("2s", sc, &t, &err) && t == 128);
  CHECK(ParseStart("0:03", sc, &t, &err) && t == 192);
  CHECK(ParseStart("t1010", sc, &t, &err) && t == 10);
  CHECK(ParseStart("1010t", sc, &t, &err) && t == 10);
  CHECK(!ParseStart("5", sc, &t, &err));      // 320 ticks > length
  CHECK(!ParseStart("t999", sc, &t, &err));   // before freeze end
  CHECK(!ParseStart("abc", sc, &t, &err) && !ParseStart("1:75", sc, &t, &err) && !ParseStart("", sc, &t, &err));

  // Player pick.
  CHECK(FindPlayer(sc, "zywoo", &err) == 0);
  CHECK(FindPlayer(sc, "3", &err) == 2);
  CHECK(FindPlayer(sc, "ap", &err) == 1);
  CHECK(FindPlayer(sc, "76561198995880877", &err) == 2);
  CHECK(FindPlayer(sc, "o", &err) == -1 && err.find("matches") != std::string::npos);  // zywoo, zont1x
  CHECK(FindPlayer(sc, "nobody", &err) == -1);
  CHECK(FindPlayer(sc, "9", &err) == -1);
  CHECK(DefaultPlayer(sc, 3, 0) == 2 && DefaultPlayer(sc, 2, 0) == 0);
  CHECK(DefaultPlayer(sc, 2, 100) == 0);

  int bt = 0, bct = 0;
  BotCounts(sc, 0, 0, &bt, &bct);
  CHECK(bt == 1 && bct == 1);
  BotCounts(sc, 2, 7, &bt, &bct);  // apEX dead at 7, human is the CT
  CHECK(bt == 1 && bct == 0);
  BotCounts(sc, -1, 0, &bt, &bct);  // watch: everyone
  CHECK(bt == 2 && bct == 1);

  // Timeline: (prev, now].
  auto due = DueBetween(sc, 0, 6);
  CHECK(due.size() == 2 && due[0].kind == Due::kGrenade && due[1].kind == Due::kDeath);
  CHECK(DueBetween(sc, 5, 6).size() == 1);
  CHECK(DueBetween(sc, 6, 199).size() == 1);
  due = DueBetween(sc, 199, 1000);
  CHECK(due.size() == 1 && due[0].kind == Due::kBomb);

  CHECK(Clock(64 * 65, 64) == "1:05");
  CHECK(Summary(sc).find("de_nuke r3") == 0 && Summary(sc).find("Alpha (T) vs Bravo (CT)") != std::string::npos);
  CHECK(PlayerList(sc) == "1 ZywOo (T), 2 apEX (T), 3 zont1x (CT)");
  CHECK(Attribution(sc) == "A vs B - Test Cup - https://example.org/demo");

  // Validation failures.
  CHECK(!Parse(With("readyup.scenario", "other"), &sc, &err));
  CHECK(!Parse(With("\"version\": 1", "\"version\": 2"), &sc, &err) && err.find("version 2") != std::string::npos);
  CHECK(!Parse(With("test_round", "Bad Id"), &sc, &err));
  CHECK(!Parse(With("\"team\": \"CT\"", "\"team\": \"X\""), &sc, &err));
  CHECK(!Parse(With("[5,5,5,90,0]", "[5,5,5,90]"), &sc, &err));
  CHECK(!Parse(With("\"type\": \"smoke\"", "\"type\": \"nuke\""), &sc, &err));
  CHECK(!Parse(With("\"player\": 2, \"type\"", "\"player\": 7, \"type\""), &sc, &err));
  CHECK(!Parse(With("[{\"t\": 0, \"hp\": 100, \"defuser\": true}]", "[{\"t\": 3}]"), &sc, &err));
  CHECK(!Parse(With("\"length\": 256", "\"length\": 0"), &sc, &err));
  CHECK(!Parse("{", &sc, &err) && !Parse("[]", &sc, &err));
  CHECK(IsValidId("nuke_r6-b") && !IsValidId("") && !IsValidId("A") && !IsValidId("a b"));

  // Every shipped scenario parses.
  int shipped = 0;
  if (DIR* d = opendir(PRACTICE_SCENARIO_DIR)) {
    while (dirent* e = readdir(d)) {
      const std::string f = e->d_name;
      if (f.size() < 6 || f.compare(f.size() - 5, 5, ".json") != 0) continue;
      Scenario s;
      std::string perr;
      const bool ok = ParseFile(std::string(PRACTICE_SCENARIO_DIR) + "/" + f, &s, &perr);
      if (!ok) std::fprintf(stderr, "%s: %s\n", f.c_str(), perr.c_str());
      CHECK(ok);
      CHECK(!ok || s.id + ".json" == f);
      CHECK(!ok || (!s.source.url.empty() && (!s.source.event.empty() || !s.source.match.empty())));
      ++shipped;
    }
    closedir(d);
  }
  std::printf("practice_scenario: %d shipped scenario(s) parsed\n", shipped);

  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("practice_scenario: all checks passed\n");
  return 0;
}
