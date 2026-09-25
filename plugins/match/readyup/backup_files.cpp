#include "readyup/backup_files.h"

#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/engine.h"
#include "readyup/persisted_match_state.h"
#include "readyup/workers.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace readyup::backup_files {
namespace {

static bool StartsWith(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin());
}

}  // namespace

std::optional<std::string> FindNewestBackupFileByPrefix(const std::string& prefix) {
  if (prefix.empty()) return std::nullopt;
  const std::string csgoDir = readyup::GetCsgoDirFromModuleDir();
  if (csgoDir.empty()) return std::nullopt;

  std::filesystem::path best;
  std::filesystem::file_time_type bestTime{};
  bool any = false;

  std::error_code ec;
  for (const auto& it : std::filesystem::recursive_directory_iterator(csgoDir, ec)) {
    if (ec) break;
    if (!it.is_regular_file(ec) || ec) continue;
    const auto p = it.path();
    if (p.extension() != ".txt") continue;
    const std::string fn = p.filename().string();
    if (!StartsWith(fn, prefix)) continue;
    const auto t = it.last_write_time(ec);
    if (ec) continue;
    if (!any || t > bestTime) {
      any = true;
      bestTime = t;
      best = p;
    }
  }

  if (!any) return std::nullopt;
  return best.filename().string();
}

void DiscoverAndPersistNewestBackupFileAsync(std::string prefix) {
  if (prefix.empty()) return;
  workers::Spawn("backup-files", [prefix = std::move(prefix)]() mutable {
    // Give the game a moment to flush the backup file.
    if (!workers::SleepFor(std::chrono::milliseconds(250))) return;
    auto fn = FindNewestBackupFileByPrefix(prefix);
    if (fn) {
      readyup::persisted_match_state::PersistBackupFile(*fn);
      if (DebugEnabled()) {
        Debug("backup_files: newest=%s\n", fn->c_str());
      }
    } else if (DebugEnabled()) {
      Debug("backup_files: no backup file found for prefix=%s\n", prefix.c_str());
    }
  });
}

}  // namespace readyup::backup_files

