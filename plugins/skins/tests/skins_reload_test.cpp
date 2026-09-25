// Offline tests for plugins/skins/skins_reload.h: when `.skins reload` is allowed. ctest
// `skins_reload`.
#include "skins_reload.h"

#include <cstdio>

using namespace skins;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

int main() {
  for (const char* m : {"", "idle", "practice", "scrim_warmup", "match_warmup"}) CHECK(SkinsReloadAllowed(m));
  for (const char* m : {"match_live", "match_knife", "knife", "postgame", "unknown", "MATCH_WARMUP"}) {
    CHECK(!SkinsReloadAllowed(m));
  }
  CHECK(SkinsReloadCooldownOk(5.0, -1));
  CHECK(!SkinsReloadCooldownOk(15.0, 10.0));
  CHECK(SkinsReloadCooldownOk(20.0, 10.0));
  std::printf("skins_reload_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
