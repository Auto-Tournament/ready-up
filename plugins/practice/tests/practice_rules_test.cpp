// Offline tests for plugins/practice/practice_rules.h. ctest `practice_rules`.
#include "practice_feedback.h"
#include "practice_rules.h"

#include <cstdio>
#include <string>

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
  std::printf("practice_rules_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
