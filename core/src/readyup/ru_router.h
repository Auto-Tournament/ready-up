#pragma once

#include <cstdint>
#include <string>

namespace readyup {

// Routes a parsed chat/console message into Ready Up commands.
// `steamid64==0` indicates server console / unknown sender.
// `slot` is the sender's engine slot when the caller knows it (ClientCommand hook), else -1.
void RouteChatCommand(uint64_t steamid64, const std::string& playerName, const std::string& text, int slot = -1);

// True if `firstToken` (e.g. ".r", ".ru") is a chat command the core itself handles.
// Plugins can never register these.
bool IsCoreChatCommand(const std::string& firstToken);

}  // namespace readyup

