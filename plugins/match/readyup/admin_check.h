#pragma once

#include <cstdint>

namespace readyup {

// Who is a Ready Up admin (readyup-match is the core's admin provider, ru_api set_admin_provider):
// - the per-match `admins` list of the loaded match config,
// - the MAT-fetched admin list (ru_admins_url),
// - the Postgres admins table (readyup_admins), cached: refreshed in the background every 30 s
//   and right after `ru admins add/remove`, so no check ever waits on the database.
// Any thread, never blocks.
bool IsReadyUpAdmin(uint64_t steamid64);

// Starts the background refresh of the database admin list (idempotent) / refreshes it now.
void AdminCacheRefreshNow();

}  // namespace readyup
