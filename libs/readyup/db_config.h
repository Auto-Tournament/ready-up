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

// Pure parsing, no core dependencies (plugins link this too). `text` is the contents of a
// readyup_db.json: {"conninfo": "..."} or discrete host/port/dbname/user/password/sslmode.
std::optional<DbConfig> ParseDbConfigText(const std::string& text, std::string* err);
std::optional<DbConfig> ReadDbConfigFile(const std::string& path, std::string* err);

// Core only (core/src/readyup/db_config_file.cpp): `readyup_db.json` next to the shim.
// Returns nullopt if missing or invalid.
std::optional<DbConfig> ReadDbConfig();

// Cached version of ReadDbConfig().
const DbConfig* DbCfg();

}  // namespace readyup

