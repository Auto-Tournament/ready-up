#pragma once

#include <cstdint>
#include <string_view>

namespace readyup {

// Converts a few common SteamID formats into SteamID64 (best-effort).
//
// Supported inputs:
// - SteamID64 decimal: "7656..."
// - Steam2: "STEAM_X:Y:Z"
// - Steam3 (individual): "[U:1:ACCOUNTID]" or "U:1:ACCOUNTID"
//
// Returns 0 on parse failure.
uint64_t ParseSteamId64Loose(std::string_view s);

}  // namespace readyup

