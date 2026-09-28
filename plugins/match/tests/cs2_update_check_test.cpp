// Offline tests for readyup/cs2_update_check.h (ctest `match_cs2_update`).
#include "readyup/cs2_update_check.h"

#include <cstdio>

using namespace readyup::cs2update;

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

int main() {
  CHECK(PatchVersionNumber("1.40.3.2") == 14032);
  CHECK(PatchVersionNumber("Patchversion 1.40.3.2") == 14032);
  CHECK(PatchVersionNumber("14032") == 14032);
  CHECK(PatchVersionNumber("") == -1);
  CHECK(PatchVersionNumber("Patchversion") == -1);
  CHECK(PatchVersionNumber("1.2.3 extra 9") == 123);

  const Answer behind = ParseAnswer(
      R"({"response":{"success":true,"up_to_date":false,"version_is_listable":true,"required_version":14035,"message":"x"}})");
  CHECK(behind.ok && !behind.upToDate && behind.required == 14035);
  const Answer fine = ParseAnswer(R"({"response":{"success":true,"up_to_date":true,"version_is_listable":true}})");
  CHECK(fine.ok && fine.upToDate);
  CHECK(!ParseAnswer("").ok);
  CHECK(!ParseAnswer("not json").ok);
  CHECK(!ParseAnswer(R"({"response":{"success":false}})").ok);
  CHECK(!ParseAnswer(R"({"response":{"success":true,"up_to_date":false}})").ok);  // no version to report
  CHECK(!ParseAnswer(R"({"nope":1})").ok);

  CHECK(CheckUrl(14032) ==
        "https://api.steampowered.com/ISteamApps/UpToDateCheck/v1/?appid=730&version=14032");

  CHECK(ShouldReport(behind, 0));
  CHECK(!ShouldReport(behind, 14035));  // once per required version
  CHECK(ShouldReport(behind, 14034));   // a newer one is reported again
  CHECK(!ShouldReport(fine, 0));
  CHECK(!ShouldReport(Answer{}, 0));

  std::printf("match_cs2_update: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
