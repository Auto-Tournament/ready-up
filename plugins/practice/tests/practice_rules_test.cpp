// Offline tests for plugins/practice/practice_rules.h. ctest `practice_rules`.
#include "practice_feedback.h"
#include "practice_rules.h"
#include "practice_tools.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace practice;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

int main() {
  for (const char* c : {".rethrow", ".rt", ".savepos", ".loadpos", ".back", ".clear", ".noflash", ".god", ".spawn",
                        ".ctspawn", ".tspawn", ".RT"}) {
    CHECK(IsToolCommand(c) && !IsBotCommand(c));
  }
  for (const char* c : {".bot", ".cbot", ".crouchbot", ".boost", ".crouchboost", ".nobots"}) CHECK(IsBotCommand(c));
  CHECK(!IsToolCommand(".r") && !IsBotCommand(".prac"));

  CHECK(ToolsAllowed(true, "default") && ToolsAllowed(true, ""));
  CHECK(!ToolsAllowed(false, "default"));
  CHECK(!ToolsAllowed(true, "valve") && !ToolsAllowed(true, " VALVE"));

  for (const char* m : {"match_warmup", "match_knife", "match_live", "knife", "postgame"}) CHECK(MatchBlocksPractice(m));
  for (const char* m : {"", "idle", "scrim_warmup", "practice"}) CHECK(!MatchBlocksPractice(m));

  CHECK(ShouldAutoEnter(true, false, "idle") && ShouldAutoEnter(true, false, ""));
  CHECK(!ShouldAutoEnter(true, true, "practice"));
  CHECK(!ShouldAutoEnter(false, false, "idle"));
  CHECK(!ShouldAutoEnter(true, false, "match_live"));

  CHECK(ParseBool("1", false) && !ParseBool("off", true) && ParseBool("?", true));

  // Feedback lines.
  CHECK(std::string(HitgroupName(1)) == "head" && std::string(HitgroupName(0)).empty());
  CHECK(FormatHit("Simpert", "Bot Adam", 27, 8, 1, "ak47", 73) ==
        "Simpert hit Bot Adam: -27 hp, -8 armor (head, ak47), 73 hp left");
  CHECK(FormatHit("", "Simpert", 12, 0, 0, "", 88) == "Simpert took -12 hp, 88 hp left");
  CHECK(FormatHit("Simpert", "Simpert", 40, 0, 0, "hegrenade", 60) == "Simpert took -40 hp (hegrenade), 60 hp left");
  CHECK(FormatBlind("Bot Adam", "Simpert", 2.44) == "Bot Adam flashed 2.4 s by Simpert");
  CHECK(FormatBlind("Simpert", "Simpert", 1.0) == "Simpert flashed 1.0 s by themselves");
  CHECK(IsSummedWeapon("hegrenade") && IsSummedWeapon("inferno") && !IsSummedWeapon("ak47"));

  GrenadeDamage gd;
  gd.Add(10.0, "Simpert", "inferno", "Bot Adam", 8);
  gd.Add(10.3, "Simpert", "inferno", "Bot Adam", 8);
  gd.Add(10.4, "Simpert", "hegrenade", "Bot Ben", 41);
  gd.Add(10.4, "Simpert", "hegrenade", "Bot Adam", 57);
  CHECK(gd.Flush(10.9, 1.0).empty());  // still burning / just exploded
  const auto lines = gd.Flush(11.5, 1.0);
  CHECK(lines.size() == 2);
  CHECK(lines.size() == 2 && lines[0] == "HE by Simpert: 98 total (Bot Adam -57, Bot Ben -41)");
  CHECK(lines.size() == 2 && lines[1] == "Fire by Simpert: 16 total (Bot Adam -16)");
  CHECK(gd.Flush(20.0, 1.0).empty());

  // ME extras (practice_tools.h).
  for (const char* c : {".throw", ".last", ".lastindex", ".throwidx", ".throwindex", ".delay", ".rethrowsmoke", ".throwdecoy",
                        ".impacts", ".traj", ".pip", ".solid", ".break", ".timer", ".bestspawn", ".worsttspawn",
                        ".bestctspawn", ".showspawns", ".hidespawns"}) {
    CHECK(IsToolCommand(c) && !IsBotCommand(c));
  }
  CHECK(!IsToolCommand(".dryrun"));  // admin command, not a tool
  CHECK(GrenadeKind("smokegrenade") == "smoke" && GrenadeKind("weapon_flashbang") == "flash");
  CHECK(GrenadeKind("hegrenade") == "hegrenade" && GrenadeKind("incgrenade") == "molotov");
  CHECK(GrenadeKind("molotov") == "molotov" && GrenadeKind("decoy") == "decoy" && GrenadeKind("ak47").empty());
  CHECK(TypedRethrowKind(".rethrowsmoke") == "smoke" && TypedRethrowKind(".throwflash") == "flash");
  CHECK(TypedRethrowKind(".thrownade") == "hegrenade" && TypedRethrowKind(".rethrowgrenade") == "hegrenade");
  CHECK(TypedRethrowKind(".throwmolotov") == "molotov" && TypedRethrowKind(".RETHROWDECOY") == "decoy");
  CHECK(TypedRethrowKind(".rethrow").empty() && TypedRethrowKind(".throwidx").empty() && TypedRethrowKind(".last").empty());

  ThrowHistory hist(3);
  CHECK(hist.Count() == 0 && !hist.Last() && !hist.At(1));
  for (int i = 1; i <= 4; ++i) {
    Throw t;
    t.pos.x = static_cast<float>(i);
    t.kind = i % 2 ? "smoke" : "flash";
    hist.Add(t);
  }
  CHECK(hist.Count() == 3);                       // capped: throw 1 dropped
  CHECK(hist.At(1) && hist.At(1)->pos.x == 2.f);  // oldest first
  CHECK(hist.At(3) && hist.At(3)->pos.x == 4.f && !hist.At(4) && !hist.At(0));
  CHECK(hist.LastOfKind("smoke") && hist.LastOfKind("smoke")->pos.x == 3.f);
  CHECK(!hist.LastOfKind("decoy"));
  hist.Last()->delay = 1.5f;
  CHECK(hist.At(3)->delay == 1.5f);

  int n = 0;
  CHECK(ParsePositiveInt("3", &n) && n == 3);
  CHECK(!ParsePositiveInt("0", &n) && !ParsePositiveInt("-1", &n) && !ParsePositiveInt("2x", &n) && !ParsePositiveInt("", &n));
  float d = 0;
  CHECK(ParseDelaySeconds("1.25", &d) && d == 1.25f);
  CHECK(!ParseDelaySeconds("0", &d) && !ParseDelaySeconds("-2", &d) && !ParseDelaySeconds("abc", &d) &&
        !ParseDelaySeconds("61", &d) && !ParseDelaySeconds("nan", &d));

  const std::vector<Vec3f> pts = {{0, 0, 0}, {100, 0, 0}, {-500, 0, 0}};
  CHECK(ClosestIndex(pts, {90, 0, 0}) == 1 && FarthestIndex(pts, {90, 0, 0}) == 2);
  CHECK(ClosestIndex({}, {0, 0, 0}) == -1 && FarthestIndex({}, {0, 0, 0}) == -1);

  CHECK(NextSolidValue(0) == 2 && NextSolidValue(1) == 2 && NextSolidValue(2) == 1);
  CHECK(FormatTimerSeconds(12.345) == "12.35" || FormatTimerSeconds(12.345) == "12.34");
  CHECK(FormatTimerSeconds(-1) == "0.00");

  std::printf("practice_rules_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
