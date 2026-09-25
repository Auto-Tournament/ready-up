#include "readyup/admin_check.h"

#include "readyup/local_store.h"
#include "readyup/logging.h"
#include "readyup/mat_admins.h"
#include "readyup/webhook.h"
#include "readyup/workers.h"

#include <atomic>
#include <chrono>

namespace readyup {
namespace {

constexpr auto kRefreshEvery = std::chrono::seconds(30);

std::atomic<bool> g_started{false};

void RefreshLoop() {
  while (!workers::ShuttingDown()) {
    local_store::ReloadAdminsIfChanged();
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
  // Fleet mode: only the platform's list (D5); standalone: admins.json.
  return local_store::FleetMode() ? local_store::IsFleetAdmin(steamid64) : local_store::IsLocalAdmin(steamid64);
}

void AdminCacheRefreshNow() {
  if (!g_started.exchange(true)) {
    if (!workers::Spawn("admin-cache", RefreshLoop)) g_started.store(false);
    return;
  }
  workers::WakeAll();  // the loop re-reads right away
}

}  // namespace readyup
