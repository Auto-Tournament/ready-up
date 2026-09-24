#pragma once

#include <cstdint>

namespace readyup {

// Unified admin check:
// - per-match `admins` list from match config
// - global MAT-fetched admins list
// - Postgres fallback
bool IsReadyUpAdmin(uint64_t steamid64);

}  // namespace readyup

