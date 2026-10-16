// Offline tests for core/src/readyup/slot_registry.h: a player who disconnected is not
// re-added by the lines CS2 logs right after the disconnect. ctest `slot_registry`.
#include "readyup/slot_registry.h"

#include <cstdio>

using namespace readyup;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static bool Present(uint64_t sid) {
  for (const auto& h : ListHumans()) {
    if (h.steamid64 == sid) return true;
  }
  return false;
}

// What client_command_hook does with one player log line.
static void Line(uint64_t sid, int userid, bool leaving, bool connectLine) {
  if (!AcceptHumanLogLine(sid, leaving, connectLine)) return;
  if (leaving) ForgetHuman(sid);
  else ObserveHuman(userid, sid, "p", -1);
}

int main() {
  const uint64_t cola = 76561198854229724ull;
  const uint64_t emil = 76561198003830440ull;

  // Never seen: any header line adds them (the plugin loaded mid-match).
  Line(cola, 4, false, false);
  CHECK(Present(cola));

  // 2026-10-16: `disconnected (reason ...)` then `triggered "Dropped_The_Bomb"` and
  // `committed suicide` for the same header. The tail must not bring them back.
  Line(cola, 4, true, false);
  CHECK(!Present(cola));
  Line(cola, 4, false, false);
  Line(cola, 4, false, false);
  CHECK(!Present(cola));

  // Someone else takes the number: unaffected.
  Line(emil, 4, false, true);
  CHECK(Present(emil));
  CHECK(ListHumans().size() == 1);

  // They connect again: back, and later lines keep them.
  Line(cola, 7, false, true);
  CHECK(Present(cola));
  Line(cola, 7, false, false);
  CHECK(Present(cola));

  // A second disconnect works the same way.
  Line(cola, 7, true, false);
  Line(cola, 7, false, false);
  CHECK(!Present(cola));

  if (g_failures == 0) std::printf("slot_registry: ok\n");
  return g_failures == 0 ? 0 : 1;
}
