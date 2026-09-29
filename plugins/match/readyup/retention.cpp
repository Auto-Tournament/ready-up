// File retention sweep (see retention.h).
#include "readyup/retention.h"

#include "readyup/backup_files.h"
#include "readyup/config.h"
#include "readyup/demo_recorder.h"
#include "readyup/fleet_bridge.h"
#include "readyup/logging.h"
#include "readyup/match_config_parser.h"
#include "readyup/persisted_match_state.h"
#include "readyup/retention_rules.h"
#include "readyup/webhook.h"
#include "readyup/workers.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace readyup::retention {
namespace {

std::atomic<bool> g_running{false};

std::vector<FileEntry> ListFiles(const std::string& dir) {
  std::vector<FileEntry> out;
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.empty() || n[0] == '.') continue;
    struct stat sb {};
    if (stat((dir + "/" + n).c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
    out.push_back(FileEntry{n, static_cast<long long>(sb.st_mtime)});
  }
  closedir(d);
  return out;
}

std::vector<std::string> Unique(std::vector<std::string> dirs) {
  std::vector<std::string> out;
  for (auto& d : dirs) {
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    if (!d.empty() && std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
  }
  return out;
}

// Deletes `names` in `dir`; counts deleted / failed.
void Delete(const std::string& dir, const std::vector<std::string>& names, int* deleted, int* failed) {
  for (const auto& n : names) {
    if (workers::ShuttingDown()) return;
    if (unlink((dir + "/" + n).c_str()) == 0) {
      ++*deleted;
      if (DebugEnabled()) Debug("retention: deleted %s/%s\n", dir.c_str(), n.c_str());
    } else if (errno != ENOENT) {
      ++*failed;
      Print("retention: could not delete %s/%s: %s\n", dir.c_str(), n.c_str(), std::strerror(errno));
    }
  }
}

}  // namespace

void SweepAsync() {
  const ReadyUpCfg cfg = Cfg();
  const int backupHours = cfg.backup_keep_hours;
  int demoHours = cfg.demo_keep_hours;
  if (demoHours > 0 && fleet_bridge::DemosUnconfirmed()) {
    Debug("retention: a streamed demo is not confirmed by the platform yet; demos are kept this time\n");
    demoHours = 0;
  }
  if (backupHours <= 0 && demoHours <= 0) return;

  // What must stay, read on the game thread.
  std::vector<uint64_t> protectedIds;
  if (const auto ctx = WebhookGetMatchContext()) protectedIds.push_back(ctx->matchid);
  if (const auto json = persisted_match_state::GetActiveMatchJson()) {
    std::string err;
    if (const auto saved = ParseWebhookMatchContextFromJson(*json, &err)) protectedIds.push_back(saved->matchid);
  }
  std::vector<std::string> keepDemos = demo::ActiveDemoFiles();
  std::vector<std::string> backupDirs = Unique(backup_files::BackupDirs());
  std::vector<std::string> demoDirs = Unique(demo::DemoDirs());

  if (g_running.exchange(true)) return;  // a sweep is still running
  const bool spawned = workers::Spawn("retention", [=]() {
    const long long now = static_cast<long long>(std::time(nullptr));
    int backups = 0, demos = 0, failed = 0;
    if (backupHours > 0) {
      for (const auto& dir : backupDirs) {
        if (workers::ShuttingDown()) break;
        Delete(dir, BackupsToDelete(ListFiles(dir), now, backupHours, protectedIds), &backups, &failed);
      }
    }
    if (demoHours > 0) {
      for (const auto& dir : demoDirs) {
        if (workers::ShuttingDown()) break;
        Delete(dir, DemosToDelete(ListFiles(dir), now, demoHours, protectedIds, keepDemos), &demos, &failed);
      }
    }
    if (backups || demos || failed) {
      Print("retention: deleted %d round backup%s (%d h) and %d demo%s (%d h)%s\n", backups, backups == 1 ? "" : "s",
            backupHours, demos, demos == 1 ? "" : "s", demoHours,
            failed ? (", " + std::to_string(failed) + " failed").c_str() : "");
    }
    g_running = false;
  });
  if (!spawned) g_running = false;
}

}  // namespace readyup::retention
