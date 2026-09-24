#include "readyup/persisted_match_state.h"

#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/postgres.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace readyup::persisted_match_state {
namespace {

static constexpr const char* kKeyMatchJson = "ru_active_match_json";
static constexpr const char* kKeyLive = "ru_active_match_live";
static constexpr const char* kKeyMap = "ru_active_map_number";
static constexpr const char* kKeyRound = "ru_active_round_number";
static constexpr const char* kKeyT1 = "ru_active_team1_score";
static constexpr const char* kKeyT2 = "ru_active_team2_score";
static constexpr const char* kKeyBackupPrefix = "ru_active_backup_prefix";
static constexpr const char* kKeyBackupFile = "ru_active_backup_file";

static void SetAsync(std::string key, std::optional<std::string> value) {
  if (!pg::Available()) return;
  if (key.empty()) return;
  std::thread([key = std::move(key), value = std::move(value)]() mutable {
    std::string err;
    if (!pg::EnsureSchema(&err)) return;
    err.clear();
    if (value.has_value()) (void)pg::SetSetting(key, *value, &err);
    else (void)pg::ClearSetting(key, &err);
  }).detach();
}

static std::optional<std::string> GetSync(const char* key) {
  if (!pg::Available()) return std::nullopt;
  std::string err;
  auto v = pg::GetSetting(key, &err);
  if (!err.empty() && DebugEnabled()) {
    Debug("persisted_match_state: GetSetting(%s) err=%s\n", key, err.c_str());
  }
  if (v && v->empty()) return std::nullopt;
  return v;
}

// Debounce snapshot writes so round_end bursts don't spam DB.
std::mutex g_snapMu;
std::atomic<bool> g_snapScheduled{false};
int g_pendingMap = 0;
int g_pendingRound = 0;
int g_pendingT1 = 0;
int g_pendingT2 = 0;

static void ScheduleSnapshotWrite() {
  bool expected = false;
  if (!g_snapScheduled.compare_exchange_strong(expected, true)) return;

  std::thread([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    int map = 0, round = 0, t1 = 0, t2 = 0;
    {
      std::lock_guard<std::mutex> lk(g_snapMu);
      map = g_pendingMap;
      round = g_pendingRound;
      t1 = g_pendingT1;
      t2 = g_pendingT2;
    }

    SetAsync(kKeyMap, std::to_string(map));
    SetAsync(kKeyRound, std::to_string(round));
    SetAsync(kKeyT1, std::to_string(t1));
    SetAsync(kKeyT2, std::to_string(t2));

    g_snapScheduled.store(false);
  }).detach();
}

}  // namespace

void PersistActiveMatchJson(std::string json) {
  if (json.empty()) return;
  SetAsync(kKeyMatchJson, std::move(json));
  // Reset per-match markers.
  SetAsync(kKeyLive, std::string("0"));
  SetAsync(kKeyMap, std::string("1"));
  SetAsync(kKeyRound, std::string("0"));
  SetAsync(kKeyT1, std::string("0"));
  SetAsync(kKeyT2, std::string("0"));
  SetAsync(kKeyBackupPrefix, std::nullopt);
  SetAsync(kKeyBackupFile, std::nullopt);
}

void ClearActiveMatch() {
  SetAsync(kKeyMatchJson, std::nullopt);
  SetAsync(kKeyLive, std::nullopt);
  SetAsync(kKeyMap, std::nullopt);
  SetAsync(kKeyRound, std::nullopt);
  SetAsync(kKeyT1, std::nullopt);
  SetAsync(kKeyT2, std::nullopt);
  SetAsync(kKeyBackupPrefix, std::nullopt);
  SetAsync(kKeyBackupFile, std::nullopt);
}

void PersistLiveFlag(bool live) {
  SetAsync(kKeyLive, live ? std::string("1") : std::string("0"));
}

void PersistSnapshot(int map_number, int round_number, int team1_score, int team2_score) {
  std::lock_guard<std::mutex> lk(g_snapMu);
  g_pendingMap = map_number;
  g_pendingRound = round_number;
  g_pendingT1 = team1_score;
  g_pendingT2 = team2_score;
  ScheduleSnapshotWrite();
}

void PersistBackupPrefix(std::string prefix) {
  if (prefix.empty()) return;
  SetAsync(kKeyBackupPrefix, std::move(prefix));
}

void PersistBackupFile(std::string filename) {
  if (filename.empty()) return;
  SetAsync(kKeyBackupFile, std::move(filename));
}

std::optional<std::string> GetActiveMatchJson() {
  return GetSync(kKeyMatchJson);
}

bool GetLiveFlag() {
  auto v = GetSync(kKeyLive);
  if (!v) return false;
  return *v == "1" || *v == "true";
}

std::optional<std::string> GetBackupFile() {
  return GetSync(kKeyBackupFile);
}

std::optional<int> GetLastRoundNumber() {
  auto v = GetSync(kKeyRound);
  if (!v) return std::nullopt;
  try {
    return std::stoi(*v);
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace readyup::persisted_match_state

