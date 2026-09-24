#pragma once

#include <optional>
#include <string>

namespace readyup::backup_files {

// Best-effort: find the newest CS2 round backup file that starts with `prefix`.
// Returns basename only (e.g. "backup_2026_..._round27.txt").
std::optional<std::string> FindNewestBackupFileByPrefix(const std::string& prefix);

// Async helper: after a short delay, find newest backup file and persist it via persisted_match_state.
void DiscoverAndPersistNewestBackupFileAsync(std::string prefix);

}  // namespace readyup::backup_files

