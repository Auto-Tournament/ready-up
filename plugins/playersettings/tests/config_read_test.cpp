// Offline tests for plugins/playersettings/config_read.h: the cvar list, the crosshair code check and
// the `player.config` payload. ctest `playersettings_config_read`.
#include "../config_read.h"

#include <algorithm>
#include <cstdio>
#include <string>

using namespace playersettings;

static int g_failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

int main() {
  const auto& cvars = ConfigCvars();
  CHECK(cvars.size() == 26);
  CHECK(std::find(cvars.begin(), cvars.end(), "sensitivity") != cvars.end());
  // Not answered by CS2: never asked.
  CHECK(std::find(cvars.begin(), cvars.end(), "cl_crosshairgap") == cvars.end());

  CHECK(IsCrosshairCode("CSGO-a2Rkq-PJ7rN-wbQYm-9AVqS-5PbYB"));
  CHECK(IsCrosshairCode("CSvbPubOq37zTGqtsPTP5QTrp5CB4xFXiKRLfzJsm49ZRe"));  // today's CS2 code
  CHECK(!IsCrosshairCode("CSvbPubOq37zTGqtsPTP5QTrp5CB4xFXiKRLfzJsm49ZR"));
  CHECK(!IsCrosshairCode("CSvbPubOq37zTGqtsPTP5QTrp5CB4xFXiKRLfzJsm49Z-e"));
  CHECK(!IsCrosshairCode("CSGO-a2Rkq-PJ7rN-wbQYm-9AVqS"));
  CHECK(!IsCrosshairCode("CSGO-a2Rkq-PJ7rN-wbQYm-9AVqS-5PbY\""));
  CHECK(!IsCrosshairCode(""));

  const std::string p = ConfigPayload(76561198000000001ull, "CSGO-a2Rkq-PJ7rN-wbQYm-9AVqS-5PbYB",
                                      {{"sensitivity", "1.25"}, {"viewmodel_fov", "68"}});
  CHECK(p ==
        "{\"steamid64\":\"76561198000000001\",\"crosshair_code\":\"CSGO-a2Rkq-PJ7rN-wbQYm-9AVqS-5PbYB\","
        "\"cvars\":{\"sensitivity\":\"1.25\",\"viewmodel_fov\":\"68\"}}");
  // No code: none sent. Odd characters are escaped, long values cut.
  const std::string q = ConfigPayload(1, "nope", {{"x", "a\"b\\c\n"}, {"y", std::string(40, '9')}});
  CHECK(q == "{\"steamid64\":\"1\",\"cvars\":{\"x\":\"a\\\"b\\\\c\\u000a\",\"y\":\"" + std::string(32, '9') + "\"}}");
  CHECK(ConfigPayload(1, "", {}) == "{\"steamid64\":\"1\",\"cvars\":{}}");

  if (g_failures) {
    std::fprintf(stderr, "config_read: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("config_read: ok\n");
  return 0;
}
