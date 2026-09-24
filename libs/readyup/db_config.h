#pragma once

#include <optional>
#include <string>

namespace readyup {

struct DbConfig {
  // libpq "conninfo" string, e.g.:
  // "host=127.0.0.1 port=5432 dbname=readyup user=readyup password=... sslmode=disable"
  std::string conninfo;

  // For log output (best-effort, strips password=...).
  std::string conninfo_sanitized;
};

// Reads `readyup_db.json` located next to `readyup.cfg` (i.e. next to the shim).
// Returns nullopt if missing or invalid.
std::optional<DbConfig> ReadDbConfig();

// Cached version of ReadDbConfig().
const DbConfig* DbCfg();

}  // namespace readyup

