#include "readyup/postgres.h"

#include "readyup/config.h"
#include "readyup/db_config.h"
#include "readyup/logging.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

#if !defined(READYUP_NO_POSTGRES)
#include <libpq-fe.h>
#endif

namespace readyup {
namespace pg {
namespace {

static void SetErr(std::string* err, const char* msg) {
  if (err) *err = msg ? msg : "";
}

#if defined(READYUP_NO_POSTGRES)

// Stubs.

#else

struct PgState {
  std::mutex mu;
  PGconn* conn = nullptr;
  bool schema_ok = false;
};

PgState& State() {
  static PgState st;
  return st;
}

static std::string LastError(PGconn* c) {
  const char* m = c ? PQerrorMessage(c) : nullptr;
  std::string s = m ? m : "unknown error";
  // PQerrorMessage often includes trailing newline.
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

static bool ConnectLocked(PgState& st, std::string* err) {
  const DbConfig* cfg = DbCfg();
  if (!cfg) {
    SetErr(err, "db not configured (readyup_db.json missing/invalid)");
    if (DebugEnabled()) {
      PrintLine("db: connect skipped (no db config).");
    }
    return false;
  }

  if (st.conn) {
    const auto status = PQstatus(st.conn);
    if (status == CONNECTION_OK) return true;
    // bad connection: reset
    if (DebugEnabled()) {
      PrintLine("db: connection not OK; reconnecting.");
    }
    PQfinish(st.conn);
    st.conn = nullptr;
    st.schema_ok = false;
  }

  if (DebugEnabled()) {
    Print("db: connecting (%s)\n", cfg->conninfo_sanitized.c_str());
  }
  st.conn = PQconnectdb(cfg->conninfo.c_str());
  if (!st.conn) {
    SetErr(err, "PQconnectdb returned null");
    return false;
  }
  if (PQstatus(st.conn) != CONNECTION_OK) {
    const std::string e = LastError(st.conn);
    if (DebugEnabled()) {
      Print("db: connect failed (%s): %s\n", cfg->conninfo_sanitized.c_str(), e.c_str());
    }
    PQfinish(st.conn);
    st.conn = nullptr;
    st.schema_ok = false;
    if (err) *err = e;
    return false;
  }

  Print("db: connected (%s)\n", cfg->conninfo_sanitized.c_str());
  if (DebugEnabled()) {
    const int ver = PQserverVersion(st.conn);
    const char* db = PQdb(st.conn);
    const char* user = PQuser(st.conn);
    const char* host = PQhost(st.conn);
    const char* port = PQport(st.conn);
    Print("db: server_version=%d db=%s user=%s host=%s port=%s\n",
          ver,
          db ? db : "(null)",
          user ? user : "(null)",
          host ? host : "(null)",
          port ? port : "(null)");
  }
  return true;
}

static bool ExecSimpleLocked(PgState& st, const char* sql, std::string* err) {
  PGresult* res = PQexec(st.conn, sql);
  if (!res) {
    if (err) *err = LastError(st.conn);
    if (DebugEnabled()) {
      Print("db: exec failed (no result): %s\n", err ? err->c_str() : "(unknown)");
    }
    return false;
  }
  const auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    if (err) *err = PQresultErrorMessage(res) ? PQresultErrorMessage(res) : LastError(st.conn);
    if (DebugEnabled()) {
      Print("db: exec failed (status=%d): %s\n", static_cast<int>(status), err ? err->c_str() : "(unknown)");
    }
    PQclear(res);
    return false;
  }
  PQclear(res);
  return true;
}

static bool EnsureSchemaLocked(PgState& st, std::string* err) {
  if (st.schema_ok) return true;

  if (DebugEnabled()) {
    PrintLine("db: ensuring schema (readyup_admins, readyup_settings).");
  }
  // Single-statement DDL (best-effort).
  const char* ddlAdmins =
      "CREATE TABLE IF NOT EXISTS readyup_admins ("
      "  steamid64 BIGINT PRIMARY KEY,"
      "  display_name TEXT NOT NULL,"
      "  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),"
      "  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()"
      ");";

  const char* ddlSettings =
      "CREATE TABLE IF NOT EXISTS readyup_settings ("
      "  key TEXT PRIMARY KEY,"
      "  value TEXT NOT NULL,"
      "  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()"
      ");";

  if (!ExecSimpleLocked(st, ddlAdmins, err)) return false;
  if (!ExecSimpleLocked(st, ddlSettings, err)) return false;
  st.schema_ok = true;
  if (DebugEnabled()) {
    PrintLine("db: schema OK.");
  }
  return true;
}

static bool ExecParamsLocked(
    PgState& st,
    const char* sql,
    int nParams,
    const char* const* paramValues,
    std::string* err,
    PGresult** outRes) {
  PGresult* res = PQexecParams(
      st.conn, sql, nParams,
      /*paramTypes=*/nullptr,
      paramValues,
      /*paramLengths=*/nullptr,
      /*paramFormats=*/nullptr,
      /*resultFormat=*/0);

  if (!res) {
    if (err) *err = LastError(st.conn);
    if (DebugEnabled()) {
      Print("db: execParams failed (no result): %s\n", err ? err->c_str() : "(unknown)");
    }
    return false;
  }

  const auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    if (err) *err = PQresultErrorMessage(res) ? PQresultErrorMessage(res) : LastError(st.conn);
    if (DebugEnabled()) {
      Print("db: execParams failed (status=%d): %s\n", static_cast<int>(status), err ? err->c_str() : "(unknown)");
    }
    PQclear(res);
    return false;
  }

  if (outRes) {
    *outRes = res;
  } else {
    PQclear(res);
  }
  return true;
}

#endif  // READYUP_NO_POSTGRES

}  // namespace

bool Available() {
#if defined(READYUP_NO_POSTGRES)
  return false;
#else
  return true;
#endif
}

bool EnsureSchema(std::string* err) {
#if defined(READYUP_NO_POSTGRES)
  SetErr(err, "postgres support not compiled in");
  return false;
#else
  auto& st = State();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!ConnectLocked(st, err)) return false;
  return EnsureSchemaLocked(st, err);
#endif
}

std::vector<AdminEntry> ListAdmins(std::string* err) {
  std::vector<AdminEntry> out;
#if defined(READYUP_NO_POSTGRES)
  SetErr(err, "postgres support not compiled in");
  return out;
#else
  auto& st = State();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!ConnectLocked(st, err)) return out;
  if (!EnsureSchemaLocked(st, err)) return out;

  const char* sql = "SELECT steamid64, display_name FROM readyup_admins ORDER BY display_name ASC;";
  PGresult* res = nullptr;
  if (!ExecParamsLocked(st, sql, 0, nullptr, err, &res)) return out;

  const int rows = PQntuples(res);
  out.reserve(rows > 0 ? static_cast<size_t>(rows) : 0u);
  for (int r = 0; r < rows; ++r) {
    const char* idStr = PQgetvalue(res, r, 0);
    const char* nameStr = PQgetvalue(res, r, 1);
    if (!idStr || !*idStr) continue;
    AdminEntry e;
    e.steamid64 = static_cast<uint64_t>(std::strtoull(idStr, nullptr, 10));
    e.display_name = nameStr ? nameStr : "";
    out.push_back(std::move(e));
  }
  PQclear(res);
  return out;
#endif
}

[[maybe_unused]] static int ParseIntOr0(const char* s) {
  if (!s || !*s) return 0;
  return static_cast<int>(std::strtol(s, nullptr, 10));
}

bool AddAdmin(uint64_t steamid64, const std::string& display_name, std::string* err) {
#if defined(READYUP_NO_POSTGRES)
  (void)steamid64;
  (void)display_name;
  SetErr(err, "postgres support not compiled in");
  return false;
#else
  if (steamid64 == 0) {
    SetErr(err, "invalid steamid64");
    return false;
  }

  auto& st = State();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!ConnectLocked(st, err)) return false;
  if (!EnsureSchemaLocked(st, err)) return false;

  const std::string idStr = std::to_string(steamid64);
  const char* params[2] = {idStr.c_str(), display_name.c_str()};
  const char* sql =
      "INSERT INTO readyup_admins(steamid64, display_name) "
      "VALUES ($1::bigint, $2::text) "
      "ON CONFLICT (steamid64) DO UPDATE SET "
      "  display_name = EXCLUDED.display_name, "
      "  updated_at = now();";
  return ExecParamsLocked(st, sql, 2, params, err, nullptr);
#endif
}

bool SetSetting(const std::string& key, const std::string& value, std::string* err) {
#if defined(READYUP_NO_POSTGRES)
  (void)key;
  (void)value;
  SetErr(err, "postgres support not compiled in");
  return false;
#else
  if (key.empty()) {
    SetErr(err, "invalid key");
    return false;
  }

  auto& st = State();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!ConnectLocked(st, err)) return false;
  if (!EnsureSchemaLocked(st, err)) return false;

  const char* params[2] = {key.c_str(), value.c_str()};
  const char* sql =
      "INSERT INTO readyup_settings(key, value) "
      "VALUES ($1::text, $2::text) "
      "ON CONFLICT (key) DO UPDATE SET "
      "  value = EXCLUDED.value, "
      "  updated_at = now();";
  return ExecParamsLocked(st, sql, 2, params, err, nullptr);
#endif
}

bool ClearSetting(const std::string& key, std::string* err) {
#if defined(READYUP_NO_POSTGRES)
  (void)key;
  SetErr(err, "postgres support not compiled in");
  return false;
#else
  if (key.empty()) {
    SetErr(err, "invalid key");
    return false;
  }

  auto& st = State();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!ConnectLocked(st, err)) return false;
  if (!EnsureSchemaLocked(st, err)) return false;

  const char* params[1] = {key.c_str()};
  const char* sql = "DELETE FROM readyup_settings WHERE key = $1::text;";
  return ExecParamsLocked(st, sql, 1, params, err, nullptr);
#endif
}

std::optional<std::string> GetSetting(const std::string& key, std::string* err) {
#if defined(READYUP_NO_POSTGRES)
  (void)key;
  SetErr(err, "postgres support not compiled in");
  return std::nullopt;
#else
  if (key.empty()) {
    SetErr(err, "invalid key");
    return std::nullopt;
  }

  auto& st = State();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!ConnectLocked(st, err)) return std::nullopt;
  if (!EnsureSchemaLocked(st, err)) return std::nullopt;

  const char* params[1] = {key.c_str()};
  const char* sql = "SELECT value FROM readyup_settings WHERE key = $1::text LIMIT 1;";
  PGresult* res = nullptr;
  if (!ExecParamsLocked(st, sql, 1, params, err, &res)) return std::nullopt;
  std::optional<std::string> out;
  if (PQntuples(res) > 0) {
    const char* v = PQgetvalue(res, 0, 0);
    out = std::string(v ? v : "");
  }
  PQclear(res);
  return out;
#endif
}

bool RemoveAdmin(uint64_t steamid64, std::string* err) {
#if defined(READYUP_NO_POSTGRES)
  (void)steamid64;
  SetErr(err, "postgres support not compiled in");
  return false;
#else
  if (steamid64 == 0) {
    SetErr(err, "invalid steamid64");
    return false;
  }

  auto& st = State();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!ConnectLocked(st, err)) return false;
  if (!EnsureSchemaLocked(st, err)) return false;

  const std::string idStr = std::to_string(steamid64);
  const char* params[1] = {idStr.c_str()};
  const char* sql = "DELETE FROM readyup_admins WHERE steamid64 = $1::bigint;";
  return ExecParamsLocked(st, sql, 1, params, err, nullptr);
#endif
}

bool IsAdmin(uint64_t steamid64, std::string* err) {
#if defined(READYUP_NO_POSTGRES)
  (void)steamid64;
  SetErr(err, "postgres support not compiled in");
  return false;
#else
  if (steamid64 == 0) return false;

  auto& st = State();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!ConnectLocked(st, err)) return false;
  if (!EnsureSchemaLocked(st, err)) return false;

  const std::string idStr = std::to_string(steamid64);
  const char* params[1] = {idStr.c_str()};
  const char* sql = "SELECT 1 FROM readyup_admins WHERE steamid64 = $1::bigint LIMIT 1;";
  PGresult* res = nullptr;
  if (!ExecParamsLocked(st, sql, 1, params, err, &res)) return false;
  const bool ok = PQntuples(res) > 0;
  PQclear(res);
  return ok;
#endif
}

bool Ping(std::string* err) {
#if defined(READYUP_NO_POSTGRES)
  SetErr(err, "postgres support not compiled in");
  return false;
#else
  auto& st = State();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!ConnectLocked(st, err)) return false;
  return ExecSimpleLocked(st, "SELECT 1;", err);
#endif
}

}  // namespace pg
}  // namespace readyup

