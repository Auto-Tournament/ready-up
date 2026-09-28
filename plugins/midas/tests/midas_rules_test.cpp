// Offline tests for plugins/midas/midas_rules.h: `midas_steamids`, `color`, `enabled`, when a
// weapon is tinted (off by default, never under the valve ruleset), the paint-kit finish and the
// best-player rule (who, when, where). ctest `midas_rules`.
#include "midas_rules.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

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
  CHECK(Paintable("weapon_knife") && Paintable("weapon_knife_t") && Paintable("weapon_bayonet"));
  CHECK(!Paintable("weapon_c4") && !Paintable("weapon_hegrenade") && !Paintable("weapon_flashbang"));
  CHECK(!Paintable("weapon_healthshot") && !Paintable("prop_physics") && !Paintable(""));

  // StatTrak: guns and knives, not the Zeus / grenades / C4.
  CHECK(StatTrakable("weapon_ak47") && StatTrakable("weapon_knife") && StatTrakable("weapon_knife_t"));
  CHECK(!StatTrakable("weapon_taser") && !StatTrakable("weapon_hegrenade") && !StatTrakable("weapon_c4") && !StatTrakable(""));
  CHECK(CountsAsKill(0, 1, 2, 3) && CountsAsKill(5, 6, 0, 3));  // unknown team counts
  CHECK(!CountsAsKill(-1, 1, 2, 3) && !CountsAsKill(1, 1, 2, 3) && !CountsAsKill(0, 1, 3, 3) && !CountsAsKill(64, 1, 2, 3));
  CHECK(StatTrakKills(true, true, 12, 30) == 12);   // the match plugin's stats when they list the player
  CHECK(StatTrakKills(true, false, 0, 7) == 7);     // not listed (e.g. a bot): own count
  CHECK(StatTrakKills(false, true, 12, 3) == 3);    // not recording (warmup, practice): own count
  CHECK(StatTrakKills(false, false, 0, -2) == 0);
  {
    const float f0 = KillEaterBits(0), f17 = KillEaterBits(17), fneg = KillEaterBits(-5);
    uint32_t u0 = 1, u17 = 0, uneg = 1;
    std::memcpy(&u0, &f0, 4);
    std::memcpy(&u17, &f17, 4);
    std::memcpy(&uneg, &fneg, 4);
    CHECK(u0 == 0 && u17 == 17 && uneg == 0);
    // econ_attr_set_by_name takes a double and narrows it back to float: the bits survive.
    const float back = static_cast<float>(static_cast<double>(f17));
    uint32_t ub = 0;
    std::memcpy(&ub, &back, 4);
    CHECK(ub == 17);
  }

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

  // skins.so per-player paint: midas_steamids plus the best player, only active with finish=auto.
  const std::set<uint64_t> paintIds = {76561198000000001ull};
  CHECK(PaintOverrideSet(true, Finish::kAuto, paintIds, 0) == paintIds);
  CHECK(PaintOverrideSet(true, Finish::kAuto, paintIds, 76561198000000009ull).size() == 2);
  CHECK(PaintOverrideSet(true, Finish::kAuto, {0ull}, 0).empty());
  CHECK(PaintOverrideSet(true, Finish::kTint, paintIds, 76561198000000009ull).empty());
  CHECK(PaintOverrideSet(false, Finish::kAuto, paintIds, 76561198000000009ull).empty());

  // Who is Midas: midas_steamids + given + the best player.
  {
    const uint64_t A = 76561198000000001ull, B = 76561198000000002ull, C = 76561198000000003ull,
                   D = 76561198000000004ull;
    const std::set<uint64_t> cfg = {A}, given = {A, B};
    CHECK(MidasReasons(true, cfg, given, C, A) == (kWhyConfig | kWhyGiven));
    CHECK(MidasReasons(true, cfg, given, C, B) == kWhyGiven);
    CHECK(MidasReasons(true, cfg, given, C, C) == kWhyBest);
    CHECK(MidasReasons(true, cfg, given, C, D) == 0);
    CHECK(MidasReasons(false, cfg, given, C, A) == 0);         // inactive: nobody
    CHECK(MidasReasons(true, {0ull}, {0ull}, 0, 0) == 0);        // SteamID64 0 never
    CHECK(EffectiveMidas(true, cfg, given, C) == (std::set<uint64_t>{A, B, C}));
    CHECK(EffectiveMidas(true, cfg, given, 0) == (std::set<uint64_t>{A, B}));
    CHECK(EffectiveMidas(true, {0ull}, {}, 0).empty());
    CHECK(EffectiveMidas(false, cfg, given, C).empty());
    CHECK(DescribeReasons(kWhyConfig | kWhyGiven | kWhyBest) == "midas_steamids, given by an admin, best player");
    CHECK(DescribeReasons(kWhyBest) == "best player" && DescribeReasons(0).empty());
    // Taking a given Midas who is also on midas_steamids: still Midas (config).
    std::set<uint64_t> g = given;
    CHECK(!ToggleGiven(&g, A) && !g.count(A));
    CHECK(MidasReasons(true, cfg, g, 0, A) == kWhyConfig);
    CHECK(ToggleGiven(&g, D) && g.count(D));
    CHECK(!ToggleGiven(&g, D) && !g.count(D));
    // given.txt round trip; junk and comments skipped.
    CHECK(ParseGivenFile(FormatGivenFile(given)) == given);
    CHECK(ParseGivenFile("# c\n\n 76561198000000002 \r\nnope\n76561198000000003") == (std::set<uint64_t>{B, C}));
    CHECK(ParseGivenFile("").empty());
  }

  // Player names (.ru midas give <player>).
  {
    const std::vector<std::string> names = {"Sivert", "sivertbot", "s1mple", "ZywOo", "Big Snax", "snaxx", "donk"};
    CHECK(ResolvePlayerName("Sivert", names).index == 0);
    CHECK(ResolvePlayerName("sivert", names).index == 0);        // case-insensitive exact beats the prefix
    CHECK(ResolvePlayerName("  zywoo ", names).index == 3);
    CHECK(ResolvePlayerName("siv", names).index == 0);           // prefix: the shortest
    CHECK(ResolvePlayerName("s1", names).index == 2);
    CHECK(ResolvePlayerName("wOo", names).index == 3);           // substring
    CHECK(ResolvePlayerName("snax", names).index == 5);          // prefix of "snaxx" first
    CHECK(ResolvePlayerName("nax", names).index == 5);           // substring: shortest ("snaxx" < "big snax")
    CHECK(ResolvePlayerName("dnk", names).index == 6);           // edit distance 1
    CHECK(ResolvePlayerName("zzzzzz", names).index == -1 && ResolvePlayerName("zzzzzz", names).ambiguous.empty());
    CHECK(ResolvePlayerName("", names).index == -1);
    CHECK(ResolvePlayerName("x", {}).index == -1);
    // A word inside the name beats a mid-word match, even a shorter name.
    const std::vector<std::string> words = {"xx Bob", "abob"};
    CHECK(ResolvePlayerName("bob", words).index == 0);
    // Ties: nothing picked, the candidates listed.
    const std::vector<std::string> twins = {"alpha", "alpho", "beta"};
    PlayerMatch m = ResolvePlayerName("alp", twins);
    CHECK(m.index == -1 && m.ambiguous.size() == 2 && m.ambiguous[0] == 0 && m.ambiguous[1] == 1);
    m = ResolvePlayerName("alphu", twins);                         // edit distance 1 to both
    CHECK(m.index == -1 && m.ambiguous.size() == 2);
    const std::vector<std::string> same = {"Player", "Player"};
    m = ResolvePlayerName("Player", same);
    CHECK(m.index == -1 && m.ambiguous.size() == 2);
    m = ResolvePlayerName("player", std::vector<std::string>{"Player", "PLAYER"});
    CHECK(m.index == -1 && m.ambiguous.size() == 2);
    CHECK(ResolvePlayerName("PLAYER", std::vector<std::string>{"Player", "PLAYER"}).index == 1);  // exact as typed
  }

  // Thrown / planted equipment -> the item it stands for.
  CHECK(EquipmentItemFor("hegrenade_projectile", false).defindex == 44);
  CHECK(EquipmentItemFor("flashbang_projectile", false).defindex == 43);
  CHECK(EquipmentItemFor("smokegrenade_projectile", false).defindex == 45);
  CHECK(EquipmentItemFor("decoy_projectile", false).defindex == 47);
  CHECK(EquipmentItemFor("molotov_projectile", false).defindex == 46);
  CHECK(EquipmentItemFor("molotov_projectile", true).defindex == 48 &&
        std::strcmp(EquipmentItemFor("molotov_projectile", true).classname, "weapon_incgrenade") == 0);
  CHECK(EquipmentItemFor("planted_c4", false).defindex == 49);
  CHECK(EquipmentItemFor("weapon_ak47", false).defindex == 0 && EquipmentItemFor("inferno", false).defindex == 0);

  std::printf("midas_rules_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
