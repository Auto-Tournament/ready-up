#pragma once

#include <cstdint>

namespace readyup {

// Admins the match plugin knows itself (its admin provider, ru_api set_admin_provider):
// - the per-match `admins` list of the loaded match config,
// - the MAT-fetched admin list (ru_admins_url),
// - fleet mode: the platform's list (admins.set, D5).
// Any thread, never blocks.
bool MatchOwnAdmin(uint64_t steamid64);

// MatchOwnAdmin, or any other admin provider through the core (the essentials plugin:
// admins.json). Any thread.
bool IsReadyUpAdmin(uint64_t steamid64);

}  // namespace readyup
