// Offline tests for plugins/midas/midas_rules.h: `midas_steamids`, `color`, `enabled`, when a
// weapon is tinted (off by default, never under the valve ruleset), the paint-kit finish and the
// best-player rule (who, when, where). ctest `midas_rules`.
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
  CHECK(ParseInt(" 42 ", 0, 0, 100) == 42 && ParseInt("101", 7, 0, 100) == 7 && ParseInt("4x", 7, 0, 100) == 7);
  CHECK(ParseInt("", 3, 0, 30) == 3 && ParseInt("-1", 3, 0, 30) == 3);
  CHECK(ParseFloat("0.15", 0.0f, 0.0f, 1.0f) == 0.15f && ParseFloat("2", 0.5f, 0.0f, 1.0f) == 0.5f);
  CHECK(ParseFloat("nan", 0.5f, 0.0f, 1.0f) == 0.5f && ParseFloat("0.1x", 0.5f, 0.0f, 1.0f) == 0.5f);

  // Finish: auto (paint kit via skins.so) by default, tint on request.
  Finish f = Finish::kTint;
  CHECK(ParseFinish("auto", &f) && f == Finish::kAuto);
  CHECK(ParseFinish(" Paint ", &f) && f == Finish::kAuto);
  CHECK(ParseFinish("TINT", &f) && f == Finish::kTint);
  CHECK(!ParseFinish("gold", &f) && f == Finish::kTint);
  CHECK(kGoldPaintKit == 1025);
  CHECK(Paintable("weapon_ak47") && Paintable("weapon_deagle") && Paintable("weapon_awp") && Paintable("weapon_taser"));
  CHECK(!Paintable("weapon_knife") && !Paintable("weapon_knife_t") && !Paintable("weapon_bayonet"));
  CHECK(!Paintable("weapon_c4") && !Paintable("weapon_hegrenade") && !Paintable("weapon_flashbang"));
  CHECK(!Paintable("weapon_healthshot") && !Paintable("prop_physics") && !Paintable(""));

  // Best player: parsing.
  BestStat bs = BestStat::kKills;
  CHECK(ParseBestStat("ADR", &bs) && bs == BestStat::kAdr);
  CHECK(ParseBestStat("kills", &bs) && bs == BestStat::kKills);
  CHECK(!ParseBestStat("rating", &bs) && bs == BestStat::kKills);
  BestWhen bw = BestWhen::kHalf;
  CHECK(ParseBestWhen("round", &bw) && bw == BestWhen::kRound);
  CHECK(ParseBestWhen(" half", &bw) && bw == BestWhen::kHalf);
  CHECK(!ParseBestWhen("map", &bw));

  // Where: scrims when on; real matches only with best_player_in_matches; never under valve;
  // only while the match plugin's stats are recording.
  CHECK(!BestPlayerAllowed(false, true, true, true, "default"));      // off by default
  CHECK(BestPlayerAllowed(true, false, true, true, "default"));       // scrim
  CHECK(!BestPlayerAllowed(true, false, true, false, "default"));     // real match: off by default
  CHECK(BestPlayerAllowed(true, true, true, false, "default"));       // ... unless in_matches=1
  CHECK(!BestPlayerAllowed(true, true, true, false, "valve"));        // never under valve
  CHECK(!BestPlayerAllowed(true, true, true, true, "Valve"));
  CHECK(!BestPlayerAllowed(true, true, false, true, "default"));      // warmup / knife / idle

  // When: round mode after min_rounds (at least one); half mode once per half from the second.
  CHECK(!PickNow(BestWhen::kRound, 2, 3, 1, 0) && PickNow(BestWhen::kRound, 3, 3, 1, 0));
  CHECK(!PickNow(BestWhen::kRound, 0, 0, 1, 0) && PickNow(BestWhen::kRound, 1, 0, 1, 0));
  CHECK(!PickNow(BestWhen::kHalf, 11, 3, 1, 0));                      // first half: nobody
  CHECK(PickNow(BestWhen::kHalf, 12, 3, 2, 0));                       // second half starts
  CHECK(!PickNow(BestWhen::kHalf, 13, 3, 2, 2));                      // already picked for it
  CHECK(PickNow(BestWhen::kHalf, 24, 3, 3, 2));                       // overtime half

  // Who.
  PlayerTotals a{76561198000000001ull, 20, 10, 2000, 20};  // ADR 100, 20 kills
  PlayerTotals b{76561198000000002ull, 25, 12, 1900, 20};  // ADR 95, 25 kills
  PlayerTotals late{76561198000000003ull, 3, 1, 330, 3};   // ADR 110, joined late
  CHECK(Adr(a) == 100.0 && Adr(PlayerTotals{}) == 0.0);
  CHECK(PickBest({a, b}, BestStat::kAdr, 0) == a.steamid64);
  CHECK(PickBest({a, b}, BestStat::kKills, 0) == b.steamid64);
  CHECK(PickBest({a, b, late}, BestStat::kAdr, 0) == late.steamid64);
  CHECK(PickBest({}, BestStat::kAdr, 0) == 0);
  // Nobody scored yet: no Midas.
  PlayerTotals z1{76561198000000004ull, 0, 1, 0, 2}, z2{76561198000000005ull, 0, 0, 0, 2};
  CHECK(PickBest({z1, z2}, BestStat::kAdr, 0) == 0 && PickBest({z1, z2}, BestStat::kKills, 0) == 0);
  // No round played (joined this round): not eligible.
  PlayerTotals fresh{76561198000000006ull, 5, 0, 500, 0};
  CHECK(PickBest({fresh, a}, BestStat::kKills, 0) == a.steamid64);
  // Ties: the other stat, then fewer deaths, then the current Midas stays, then lowest SteamID.
  PlayerTotals t1{76561198000000011ull, 10, 5, 1000, 10}, t2{76561198000000012ull, 12, 5, 1000, 10};
  CHECK(PickBest({t1, t2}, BestStat::kAdr, 0) == t2.steamid64);
  t2.kills = 10;
  t2.deaths = 4;
  CHECK(PickBest({t1, t2}, BestStat::kAdr, 0) == t2.steamid64);
  t2.deaths = 5;
  CHECK(PickBest({t1, t2}, BestStat::kKills, 0) == t1.steamid64);
  CHECK(PickBest({t1, t2}, BestStat::kKills, t2.steamid64) == t2.steamid64);
  CHECK(PickBest({t2, t1}, BestStat::kAdr, 0) == t1.steamid64);  // order does not matter

  std::printf("midas_rules_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
