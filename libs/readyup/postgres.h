#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace readyup {

struct AdminEntry {
  uint64_t steamid64 = 0;
  std::string display_name;
};

// Postgres-backed global admin store.
//
// Best-effort behavior:
// - If Postgres support is not compiled in, functions return false/empty and set err.
// - If DB config is missing/unreachable, functions return false/empty and set err.
namespace pg {

bool Available();

// Connects if needed and runs `SELECT 1`. Blocking (network I/O): never call on the game thread.
bool Ping(std::string* err);

bool EnsureSchema(std::string* err);
std::vector<AdminEntry> ListAdmins(std::string* err);

// Generic key/value settings store (for persisting RU initialization settings).
bool SetSetting(const std::string& key, const std::string& value, std::string* err);
bool ClearSetting(const std::string& key, std::string* err);
std::optional<std::string> GetSetting(const std::string& key, std::string* err);

// Upserts display_name for steamid64.
bool AddAdmin(uint64_t steamid64, const std::string& display_name, std::string* err);
bool RemoveAdmin(uint64_t steamid64, std::string* err);

// Convenience check for auth decisions.
bool IsAdmin(uint64_t steamid64, std::string* err);

}  // namespace pg
}  // namespace readyup

