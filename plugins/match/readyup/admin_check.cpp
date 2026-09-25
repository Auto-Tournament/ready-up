#include "readyup/admin_check.h"

#include "readyup/logging.h"
#include "readyup/mat_admins.h"
#include "readyup/postgres.h"
#include "readyup/webhook.h"
#include "readyup/workers.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_set>

namespace readyup {
namespace {

constexpr auto kRefreshEvery = std::chrono::seconds(30);

std::mutex g_mu;
std::unordered_set<uint64_t> g_dbAdmins;  // last successful read of the admins table
std::atomic<bool> g_started{false};

void RefreshLoop() {
  while (!workers::ShuttingDown()) {
    std::string err;
    if (pg::EnsureSchema(&err)) {
      err.clear();
      const auto list = pg::ListAdmins(&err);
      if (err.empty()) {
        std::unordered_set<uint64_t> ids;
        for (const auto& a : list) ids.insert(a.steamid64);
        std::lock_guard<std::mutex> lk(g_mu);
        g_dbAdmins.swap(ids);
      } else {
        Debug("admins: refresh failed: %s\n", err.c_str());
      }
    }
    if (!workers::SleepFor(kRefreshEvery)) return;
  }
}

}  // namespace

bool IsReadyUpAdmin(uint64_t steamid64) {
  if (steamid64 == 0) return false;
  if (auto ctx = WebhookGetMatchContext()) {
    if (ctx->admins.count(steamid64)) return true;
  }
  if (mat_admins::IsMatAdmin(steamid64)) return true;
  std::lock_guard<std::mutex> lk(g_mu);
  return g_dbAdmins.count(steamid64) != 0;
}

void AdminCacheRefreshNow() {
  if (!pg::Available()) return;
  if (!g_started.exchange(true)) {
    if (!workers::Spawn("admin-cache", RefreshLoop)) g_started.store(false);
    return;
  }
  workers::WakeAll();  // the loop re-reads right away
}

}  // namespace readyup
