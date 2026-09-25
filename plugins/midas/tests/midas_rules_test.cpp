// Offline tests for plugins/midas/midas_rules.h: `midas_steamids`, `color`, `enabled`, and when
// a weapon is tinted (off by default, never under the valve ruleset). ctest `midas_rules`.
#include "midas_rules.h"

#include <cstdio>

using namespace midas;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

int main() {
  int bad = -1;
  auto ids = ParseSteamIds("76561198000000001, 76561198000000002;76561198000000003  nope 123", &bad);
  CHECK(ids.size() == 3 && ids.count(76561198000000002ull) && bad == 2);
  CHECK(ParseSteamIds("", &bad).empty() && bad == 0);
  CHECK(ParseSteamIds("7656119800000000", &bad).empty() && bad == 1);    // 16 digits
  CHECK(ParseSteamIds("12345678901234567", &bad).empty() && bad == 1);   // not a SteamID64
  CHECK(ParseSteamIds("7656119800000000x", &bad).empty() && bad == 1);

  Rgba c;
  CHECK(ParseColor("255,200,40", &c) && c == kGold);
  CHECK(ParseColor(" 10, 20 ,30,40 ", &c) && c.r == 10 && c.g == 20 && c.b == 30 && c.a == 40);
  c = kGold;
  CHECK(!ParseColor("256,0,0", &c) && c == kGold);
  CHECK(!ParseColor("1,2", &c) && !ParseColor("1,2,3,4,5", &c) && !ParseColor("a,b,c", &c) && !ParseColor("", &c));
  CHECK(!ParseColor("1,,3", &c) && !ParseColor("-1,2,3", &c));

  CHECK(ParseBool("1", false) && ParseBool("Yes", false) && !ParseBool("off", true) && ParseBool("?", true));

  // Off by default (enabled=0), never under valve, whatever the case.
  CHECK(!Active(false, "default"));
  CHECK(Active(true, "default") && Active(true, ""));
  CHECK(!Active(true, "valve") && !Active(true, " Valve "));

  const auto list = ParseSteamIds("76561198000000001", nullptr);
  CHECK(ShouldTint(true, list, 76561198000000001ull));
  CHECK(!ShouldTint(true, list, 76561198000000002ull));
  CHECK(!ShouldTint(false, list, 76561198000000001ull));
  CHECK(!ShouldTint(true, list, 0));
  std::printf("midas_rules_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
