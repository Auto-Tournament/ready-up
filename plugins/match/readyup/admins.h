#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace readyup {

// Handles `ru admins ...` command.
// `senderSteamid64==0` means server console.
void HandleAdminsCommand(uint64_t senderSteamid64, const std::string& senderName, const std::vector<std::string>& args);

}  // namespace readyup

