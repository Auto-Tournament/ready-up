#pragma once

#include <optional>
#include <string>
#include <vector>

namespace readyup::backup_files {

// Where CS2 writes mp_backup_round_file backups (and reads mp_backup_restore_load_file from): the
// first Game search path of gameinfo.gi, i.e. csgo/readyup/ on Ready Up servers (observed),
// csgo/addons/metamod/ when a Metamod line comes first (observed on a csm-managed install), csgo/
// otherwise. Any thread.
std::vector<std::string> BackupDirs();

// Best-effort: find the newest CS2 round backup file that starts with `prefix`.
// Returns basename only (e.g. "backup_2026_..._round27.txt").
std::optional<std::string> FindNewestBackupFileByPrefix(const std::string& prefix);

// Async helper: after a short delay, find newest backup file and persist it via persisted_match_state.
void DiscoverAndPersistNewestBackupFileAsync(std::string prefix);

}  // namespace readyup::backup_files

