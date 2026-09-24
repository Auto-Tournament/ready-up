#pragma once

#include <cstdint>
#include <string>

namespace readyup {

// Routes a parsed chat/console message into ReadyUp commands.
// `steamid64==0` indicates server console / unknown sender.
void RouteChatCommand(uint64_t steamid64, const std::string& playerName, const std::string& text);

// True if `firstToken` (e.g. ".r", ".ru") is a chat command the core itself handles.
// Plugins can never register these.
bool IsCoreChatCommand(const std::string& firstToken);

}  // namespace readyup

