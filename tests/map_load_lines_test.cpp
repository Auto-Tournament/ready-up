// Offline tests for core/src/readyup/map_load_lines.h: one map start per level load, a same-map
// reload included, in the line order CS2 prints (instance-2 console, 2026-11-26). ctest
// `map_load_lines`.
#include "readyup/map_load_lines.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace readyup;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

// Feeds lines like the core does; the current map follows the starts sent, as in PostEvent.
static std::vector<std::string> Run(MapLoadLines& m, std::string& current, const std::vector<std::string>& lines) {
  std::vector<std::string> out;
  for (const auto& l : lines) {
    const auto s = m.Observe(l, current);
    if (!s.start) continue;
    out.push_back(s.map + (s.new_load ? "+" : ""));
    current = s.map;
  }
  return out;
}

int main() {
  MapLoadLines m;
  std::string current;
  // Boot: `map de_dust2`, then the addons plugin's changelevel to the same map (mounts the addon).
  const auto a = Run(m, current,
                     {
                         "Loading map \"de_dust2\"",
                         "CHostStateMgr::QueueNewRequest( Loading (de_dust2), 2 )",
                         "SV:  Level loading started for 'de_dust2'",
                         "L 11/26/2026 - 08:41:24: Loading map \"de_dust2\"",
                         "changelevel \"de_dust2\"",
                         "SV:  Level loading started for 'de_dust2'",
                         "L 11/26/2026 - 08:41:29: Loading map \"de_dust2\"",
                     });
  CHECK(a.size() == 2);
  CHECK(a.size() == 2 && a[0] == "de_dust2" && a[1] == "de_dust2+");

  // A map change: one start.
  const auto b = Run(m, current,
                     {"SV:  Level loading started for 'de_mirage'", "L 11/26/2026 - 09:00:00: Loading map \"de_mirage\""});
  CHECK(b.size() == 1 && b[0] == "de_mirage+");

  // No level lines (the log file reader): a new map starts on its "Loading map" line; its
  // "Started map" does not.
  MapLoadLines f;
  std::string cur2;
  const auto c = Run(f, cur2,
                     {"L 11/26/2026 - 09:00:00: Loading map \"de_nuke\"", "L 11/26/2026 - 09:00:05: Started map \"de_nuke\"",
                      "L 11/26/2026 - 09:10:00: Loading map \"de_inferno\""});
  CHECK(c.size() == 2 && c[0] == "de_nuke" && c[1] == "de_inferno");

  // Not map starts.
  MapLoadLines g;
  CHECK(!g.Observe("SV:  Level loading started for ''", "").start);
  CHECK(!g.Observe("Loading map \"\"", "").start);
  CHECK(!g.Observe("L 11/26/2026 - 09:00:00: World triggered \"Round_Start\"", "de_nuke").start);

  if (g_failures) {
    std::fprintf(stderr, "map_load_lines: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("map_load_lines: ok\n");
  return 0;
}
