#pragma once

// Steam Workshop (ISteamUGC) through Steam's flat C API in libsteam_api.so, the library CS2
// already loads: exported functions with stable names, so no signature scanning and nothing a
// CS2 update breaks unless Valve changes the Steamworks SDK interface version, which is probed.

#include <cstdint>
#include <string>

namespace readyup::steam_ugc {

// ISteamUGC::GetItemDownloadInfo for the game server's UGC interface. False when Steam has no
// download info for the item (or the interface is unavailable).
bool DownloadProgress(uint64_t workshopId, uint64_t* downloaded, uint64_t* total);

// `ru selftest` line: which accessor resolved, or why none did.
std::string Status();

}  // namespace readyup::steam_ugc
