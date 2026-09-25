#include "readyup/admin_check.h"

#include "readyup/mat_admins.h"
#include "readyup/plugin_loader.h"
#include "readyup/postgres.h"
#include "readyup/webhook.h"

namespace readyup {

bool IsReadyUpAdmin(uint64_t steamid64) {
  if (steamid64 == 0) return false;

  // A plugin admin provider (set_admin_provider) answers first.
  if (const int v = plugins::PluginAdminVerdict(steamid64); v >= 0) return v == 1;

  if (auto ctx = WebhookGetMatchContext()) {
    if (ctx->admins.find(steamid64) != ctx->admins.end()) {
      return true;
    }
  }

  if (mat_admins::IsMatAdmin(steamid64)) return true;

  std::string err;
  if (!pg::Available()) return false;
  if (!pg::EnsureSchema(&err)) return false;
  err.clear();
  return pg::IsAdmin(steamid64, &err);
}

}  // namespace readyup

