// The core's readyup_db.json (next to the shim). Parsing is shared with plugins
// (libs/readyup/db_config.cpp); this file only locates, loads and caches it for the core.
#include "readyup/db_config.h"

#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/path.h"

#include <mutex>
#include <string>

namespace readyup {
namespace {

std::once_flag g_dbCfgOnce;
std::optional<DbConfig> g_dbCfg;

}  // namespace

std::optional<DbConfig> ReadDbConfig() {
  const std::string dir = GetThisModuleDir();
  if (dir.empty()) return std::nullopt;
  const std::string path = dir + "/readyup_db.json";
  std::string err;
  auto cfg = ReadDbConfigFile(path, &err);
  if (!cfg && err.rfind("missing", 0) != 0) Print("db config: %s.\n", err.c_str());
  return cfg;
}

const DbConfig* DbCfg() {
  std::call_once(g_dbCfgOnce, []() {
    if (DebugEnabled()) {
      const std::string dir = GetThisModuleDir();
      if (!dir.empty()) {
        Print("db config: loading from: %s/readyup_db.json\n", dir.c_str());
      } else {
        PrintLine("db config: loading from: (unknown module dir)");
      }
    }

    g_dbCfg = ReadDbConfig();
    if (g_dbCfg) {
      Print("db config: loaded (conninfo=%s)\n", g_dbCfg->conninfo_sanitized.c_str());
    } else {
      if (DebugEnabled()) {
        PrintLine("db config: not configured (readyup_db.json missing or invalid).");
      }
    }
  });
  return g_dbCfg ? &(*g_dbCfg) : nullptr;
}

}  // namespace readyup
