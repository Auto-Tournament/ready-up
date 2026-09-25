// Offline tests for plugins/practice/practice_rules.h. ctest `practice_rules`.
#include "practice_rules.h"

#include <cstdio>

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
  std::printf("practice_rules_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
