#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace readyup::mat_admins {

// Configure MAT admins JSON URL.
// - Empty string or "clear" disables MAT-admin fetching and clears the cached list.
void ConfigureAdminsUrl(std::string urlOrClear);

// Configure refresh interval (seconds). Clamped to 10..3600.
void ConfigureRefreshSeconds(int seconds);

// Set/clear optional Bearer token for admins fetch requests.
// This is typically the same token set via `ru_match_token`.
void SetBearerToken(std::optional<std::string> token);

// Returns true if the SteamID64 is present in the current MAT-fetched admin set.
bool IsMatAdmin(uint64_t steamid64);

// Trigger an immediate best-effort refetch (async via background thread).
// Safe to call even if no URL is configured.
void RefreshNow();

// Debug helpers (best-effort; values are snapshots).
std::string AdminsUrl();
int RefreshSeconds();
bool LastFetchOk();

}  // namespace readyup::mat_admins

