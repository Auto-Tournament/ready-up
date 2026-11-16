// Offline tests for kills to the platform (readyup/kill_forward.h). ctest `match_kill_forward`.
#include "readyup/kill_forward.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace readyup;
using namespace readyup::kill_forward;

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

static void TestBots() {
  const auto k = ParseKillLogLine(
      "L 11/14/2026 - 18:00:00: \"ZywOo<3><BOT><CT>\" [-112 2045 -44] killed \"donk<8><BOT><TERRORIST>\" "
      "[288 -781 -167] with \"ak47\" (headshot)");
  CHECK(k.has_value());
  if (!k) return;
  CHECK(k->attacker.name == "ZywOo" && k->attacker.steamid64 == 0 && k->attacker.side == "ct");
  CHECK(k->victim.name == "donk" && k->victim.side == "t");
  CHECK(k->attacker.x == -112 && k->attacker.y == 2045 && k->victim.z == -167);
  CHECK(k->weapon == "ak47" && k->headshot);
}

static void TestHumans() {
  const auto k = ParseKillLogLine(
      "L 11/14/2026 - 18:00:01: \"a<b> c<2><[U:1:123]><TERRORIST>\" [1 2 3] killed \"Sox<5><[U:1:456]><CT>\" "
      "[4 5 6] with \"usp_silencer\"");
  CHECK(k.has_value());
  if (!k) return;
  CHECK(k->attacker.name == "a<b> c" && k->attacker.steamid64 == 76561197960265728ULL + 123);
  CHECK(k->victim.steamid64 == 76561197960265728ULL + 456 && !k->headshot && k->weapon == "usp_silencer");
  const auto d = EventData(*k, 1794670000000LL, std::nan(""));
  CHECK(d.Find("weapon") && d.Find("weapon")->AsString() == "usp_silencer");
  CHECK(!d.Find("game_time"));  // unknown: left out
  const auto t = EventData(*k, 1794670000000LL, 2590.5);
  CHECK(t.Find("game_time") && t.Find("game_time")->AsDouble() == 2590.5);
  CHECK(d.Find("attacker") && d.Find("attacker")->Find("steamid64")->AsString() == "76561197960265851");
}

static void TestNotKills() {
  CHECK(!ParseKillLogLine("L 11/14/2026 - 18:00:02: \"Sox<5><[U:1:456]><CT>\" say \"killed \\\"x\\\"\""));
  CHECK(!ParseKillLogLine("L 11/14/2026 - 18:00:03: \"Sox<5><[U:1:456]><CT>\" [1 2 3] killed other \"chicken<77>\" [4 5 6] with \"knife\""));
  CHECK(!ParseKillLogLine("L 11/14/2026 - 18:00:04: \"Sox<5><[U:1:456]><CT>\" [1 2 3] committed suicide with \"world\""));
  CHECK(!ParseKillLogLine(""));
}

int main() {
  TestBots();
  TestHumans();
  TestNotKills();
  std::printf("match_kill_forward: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
