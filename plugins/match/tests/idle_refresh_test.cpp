// Offline tests for readyup/idle_refresh.h IdleRefreshDue: when an idle server loads its map
// again. ctest `match_idle_refresh`.
#include "readyup/idle_refresh.h"

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

int main() {
  constexpr double H = 3600.0;
  const double load = 1000.0;
  // 12 h default: not before, due at and after.
  CHECK(!IdleRefreshDue(load + 11.9 * H, load, -1, 12, false, 0));
  CHECK(IdleRefreshDue(load + 12 * H, load, -1, 12, false, 0));
  CHECK(IdleRefreshDue(load + 30 * H, load, -1, 12, false, 0));
  // 0 = off, whatever the uptime.
  CHECK(!IdleRefreshDue(load + 1000 * H, load, -1, 0, false, 0));
  // A loaded match (live or not) or anyone connected blocks it.
  CHECK(!IdleRefreshDue(load + 30 * H, load, -1, 12, true, 0));
  CHECK(!IdleRefreshDue(load + 30 * H, load, -1, 12, false, 1));
  // No map start seen yet.
  CHECK(!IdleRefreshDue(load + 30 * H, -1, -1, 12, false, 0));
  // An attempt that did not change map: retried after kIdleRefreshRetrySeconds, not before.
  const double t = load + 13 * H;
  CHECK(!IdleRefreshDue(t + 60, load, t, 12, false, 0));
  CHECK(IdleRefreshDue(t + kIdleRefreshRetrySeconds, load, t, 12, false, 0));
  // Other hour settings.
  CHECK(!IdleRefreshDue(load + 0.5 * H, load, -1, 1, false, 0));
  CHECK(IdleRefreshDue(load + 1 * H, load, -1, 1, false, 0));
  std::printf("idle_refresh_test: %s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
