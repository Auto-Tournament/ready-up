// Idle map refresh (see idle_refresh.h).
#include "readyup/idle_refresh.h"

#include "readyup/config.h"
#include "readyup/engine.h"
#include "readyup/logging.h"
#include "readyup/map_names.h"
#include "readyup/match_console.h"
#include "readyup/match_state.h"
#include "readyup/players.h"
#include "readyup/webhook.h"

namespace readyup {
namespace {

double g_lastMapLoad = -1;
double g_lastAttempt = -1;
double g_nextCheck = 0;

}  // namespace

void IdleRefreshOnMapStart(double now) {
  g_lastMapLoad = now;
  g_lastAttempt = -1;
}

void IdleRefreshFrame(double now) {
  if (now < g_nextCheck) return;
  g_nextCheck = now + 5.0;
  if (g_lastMapLoad < 0) g_lastMapLoad = now;  // loaded mid-map: count from here
  const int hours = Cfg().idle_map_refresh_hours;
  const bool matchLoaded = static_cast<bool>(WebhookGetMatchContext());
  const int humans = static_cast<int>(ListHumans().size());
  if (!IdleRefreshDue(now, g_lastMapLoad, g_lastAttempt, hours, matchLoaded, humans)) return;
  g_lastAttempt = now;
  const std::string map = MatchStateGet().current_map;
  const std::string entry = mapnames::ReloadEntry(map);
  if (entry.empty()) {
    Print("idle-refresh: %d h on this map, but the current map is not known; not reloading\n", hours);
    return;
  }
  Print("idle-refresh: %.1f h on %s with nobody connected and no match loaded; loading it again\n",
        (now - g_lastMapLoad) / 3600.0, entry.c_str());
  (void)LoadMapEntry(entry);
}

}  // namespace readyup
