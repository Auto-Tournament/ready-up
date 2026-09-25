#pragma once

#include <cstdint>

namespace readyup {

// Who is a Ready Up admin (readyup-match is the core's admin provider, ru_api set_admin_provider):
// - the per-match `admins` list of the loaded match config,
// - the MAT-fetched admin list (ru_admins_url),
// - standalone: admins.json in the plugin data dir (local_store.h), re-read in the background
//   every 30 s when it changed on disk; fleet mode: the platform's list (admins.set, D5) instead.
// Any thread, never blocks.
bool IsReadyUpAdmin(uint64_t steamid64);

// Starts the background re-read of admins.json (idempotent) / re-reads it now.
void AdminCacheRefreshNow();

}  // namespace readyup
