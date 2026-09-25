#include "readyup/persisted_match_state.h"

#include "readyup/local_store.h"

#include <optional>
#include <string>

namespace readyup::persisted_match_state {
namespace {

// Keys in state.json "settings" (local_store.h). Same names the old readyup_settings table used,
// so scripts/migrate-postgres-to-json.py copies them over unchanged.
static constexpr const char* kKeyMatchJson = "ru_active_match_json";
static constexpr const char* kKeyLive = "ru_active_match_live";
static constexpr const char* kKeyMap = "ru_active_map_number";
static constexpr const char* kKeyRound = "ru_active_round_number";
static constexpr const char* kKeyT1 = "ru_active_team1_score";
static constexpr const char* kKeyT2 = "ru_active_team2_score";
static constexpr const char* kKeyBackupPrefix = "ru_active_backup_prefix";
static constexpr const char* kKeyBackupFile = "ru_active_backup_file";

// Memory at once; the store's writer thread saves (coalesced, so round_end bursts stay cheap).
static void SetAsync(const char* key, std::optional<std::string> value) {
  local_store::SetSetting(key, std::move(value));
}

static std::optional<std::string> GetSync(const char* key) { return local_store::GetSetting(key); }

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
  for (const char* k : {kKeyMatchJson, kKeyLive, kKeyMap, kKeyRound, kKeyT1, kKeyT2, kKeyBackupPrefix, kKeyBackupFile}) {
    SetAsync(k, std::nullopt);
  }
}

void PersistLiveFlag(bool live) {
  SetAsync(kKeyLive, live ? std::string("1") : std::string("0"));
}

void PersistSnapshot(int map_number, int round_number, int team1_score, int team2_score) {
  SetAsync(kKeyMap, std::to_string(map_number));
  SetAsync(kKeyRound, std::to_string(round_number));
  SetAsync(kKeyT1, std::to_string(team1_score));
  SetAsync(kKeyT2, std::to_string(team2_score));
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
