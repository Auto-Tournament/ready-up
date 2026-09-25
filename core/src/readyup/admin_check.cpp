#include "readyup/admin_check.h"

#include "readyup/plugin_loader.h"

namespace readyup {

bool IsReadyUpAdmin(uint64_t steamid64) {
  if (steamid64 == 0) return false;
  return plugins::PluginAdminVerdict(steamid64) == 1;
}

}  // namespace readyup
