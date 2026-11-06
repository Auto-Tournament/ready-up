// walkover_logic.h: the walkover choice's HUD screen and countdown.
#include "readyup/walkover_logic.h"

#include <cstdio>
#include <string>

using namespace readyup;

static int g_checks = 0, g_failures = 0;
#define CHECK(x)                                                   \
  do {                                                             \
    ++g_checks;                                                    \
    if (!(x)) {                                                    \
      ++g_failures;                                                \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); \
    }                                                              \
  } while (0)

static bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

int main() {
  WalkoverView v;
  v.shortName = "Discord|Test B";
  v.fullName = "Check-in Test";
  v.secondsLeft = 95;
  v.onFullTeam = true;
  v.chooser = true;
  std::string s = WalkoverSpec(v, "en");
  CHECK(Has(s, "btn=Take the walkover|green||.walkover\n"));
  CHECK(Has(s, "btn=Play them short|ghost||.play\n"));
  CHECK(Has(s, "mouse=on\n"));  // forced: the choice can't be skipped
  CHECK(Has(s, "timer=95\n"));
  CHECK(Has(s, "kind=DiscordTest B is short\n"));  // '|' would split the field

  v.chooser = false;
  s = WalkoverSpec(v, "en");
  CHECK(Has(s, "title=Your captain chooses\n"));
  CHECK(!Has(s, "btn="));
  CHECK(!Has(s, "mouse="));

  v.onFullTeam = false;
  v.onShortTeam = true;
  s = WalkoverSpec(v, "en");
  CHECK(Has(s, "title=Check-in Test chooses\n"));
  CHECK(!Has(s, "btn="));

  v.onShortTeam = false;
  v.secondsLeft = -3;
  s = WalkoverSpec(v, "en");
  CHECK(Has(s, "timer=0\n"));
  CHECK(!Has(s, "btn="));

  CHECK(SecondsUntil(0, 1000) == -1);
  CHECK(SecondsUntil(10'000, 1000) == 9);
  CHECK(SecondsUntil(10'001, 1000) == 10);  // rounds up: 0 only once it is over
  CHECK(SecondsUntil(500, 1000) == 0);

  const auto d = WalkoverChoiceData(76561199486434142ULL, "Simpert", true);
  CHECK(d.Dump().find("\"take\":true") != std::string::npos);
  CHECK(d.Dump().find("\"steamid64\":\"76561199486434142\"") != std::string::npos);

  std::printf("walkover: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
