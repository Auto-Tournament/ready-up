#include "readyup/admin_check.h"

#include "readyup/host.h"
#include "readyup/local_store.h"
#include "readyup/mat_admins.h"
#include "readyup/webhook.h"

namespace readyup {

bool MatchOwnAdmin(uint64_t steamid64) {
  if (steamid64 == 0) return false;
  if (auto ctx = WebhookGetMatchContext()) {
    if (ctx->admins.count(steamid64)) return true;
  }
  if (mat_admins::IsMatAdmin(steamid64)) return true;
  // Fleet mode: the platform's list (D5). Standalone admins.json is the essentials plugin's.
  return local_store::FleetMode() && local_store::IsFleetAdmin(steamid64);
}

bool IsReadyUpAdmin(uint64_t steamid64) {
  if (steamid64 == 0) return false;
  if (MatchOwnAdmin(steamid64)) return true;
  // The other admin providers (essentials: admins.json) through the core.
  const ru_api* a = host::Api();
  return a && a->is_admin(a->self, steamid64) == 1;
}

}  // namespace readyup
