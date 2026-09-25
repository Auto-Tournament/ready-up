#include "readyup/pause_state.h"

#include <chrono>
#include <mutex>
#include <string>

namespace readyup {
namespace {

std::mutex g_mu;
bool g_paused = false;
bool g_t1 = false;
bool g_t2 = false;
std::string g_type, g_by;
WebhookTeam g_team = WebhookTeam::Unknown;
std::chrono::steady_clock::time_point g_pauseStart{};

PauseSnapshot SnapshotLocked() {
  PauseSnapshot s;
  s.paused = g_paused;
  s.team1_ready_to_unpause = g_t1;
  s.team2_ready_to_unpause = g_t2;
  if (g_paused) {
    s.type = g_type;
    s.by = g_by;
    s.team = g_team;
  }
  return s;
}

}  // namespace

void PauseStateOnPaused(const char* type, const std::string& by, WebhookTeam team) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_paused = true;
  g_type = type && *type ? type : "tactical";
  g_by = by;
  g_team = team;
  g_t1 = false;
  g_t2 = false;
  g_pauseStart = std::chrono::steady_clock::now();
}

void PauseStateOnUnpaused() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_paused = false;
  g_t1 = false;
  g_t2 = false;
  g_pauseStart = {};
}

PauseSnapshot PauseStateRequestUnpause(WebhookTeam team) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (team == WebhookTeam::Team1) g_t1 = true;
  else if (team == WebhookTeam::Team2) g_t2 = true;
  return SnapshotLocked();
}

PauseSnapshot PauseStateGet() {
  std::lock_guard<std::mutex> lk(g_mu);
  return SnapshotLocked();
}

void PauseStateSave(PauseSnapshot* snap, long long* startTicks) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (snap) *snap = SnapshotLocked();
  if (startTicks) *startTicks = static_cast<long long>(g_pauseStart.time_since_epoch().count());
}

void PauseStateRestore(const PauseSnapshot& snap, long long startTicks) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_paused = snap.paused;
  g_type = snap.type;
  g_by = snap.by;
  g_team = snap.team;
  g_t1 = snap.team1_ready_to_unpause;
  g_t2 = snap.team2_ready_to_unpause;
  g_pauseStart = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(startTicks));
}

int PauseStatePauseDurationSeconds() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_paused) return 0;
  if (g_pauseStart.time_since_epoch().count() == 0) return 0;
  const auto now = std::chrono::steady_clock::now();
  const auto s = std::chrono::duration_cast<std::chrono::seconds>(now - g_pauseStart).count();
  if (s < 0) return 0;
  if (s > 24 * 3600) return 24 * 3600;
  return static_cast<int>(s);
}

}  // namespace readyup

