// What the playersettings plugin reads from each player's game once per map, and the fleet message
// it sends (`player.config`, server -> platform). Pure: tested in tests/config_read_test.cpp.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace playersettings {

// The cvars the profile's Config tab shows (the platform's CONFIG_CVARS, api/src/integrations/cs2/
// playerConfig.ts): mouse, crosshair, viewmodel, HUD and radar, fps_max. CS2 does not answer for
// cl_crosshairgap, cl_crosshair_outlinethickness, cl_crosshaircolor, cl_crosshairusealpha and
// cl_crosshairgap_useweaponvalue: the crosshair code carries those.
const std::vector<std::string>& ConfigCvars();

// "CSGO-xxxxx-xxxxx-xxxxx-xxxxx-xxxxx" (letters and digits).
bool IsCrosshairCode(const std::string& code);

// The `player.config` payload: {"steamid64":"…","crosshair_code":"…","cvars":{"name":"value",…}}.
// The code only when it is one; values are JSON-escaped and cut to 32 bytes.
std::string ConfigPayload(uint64_t steamid64, const std::string& code, const std::map<std::string, std::string>& cvars);

}  // namespace playersettings
