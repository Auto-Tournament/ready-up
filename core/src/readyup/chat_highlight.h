#pragma once

#include <string>

namespace readyup {

// Colors the part of a chat line a player looks for: `.commands` in green. A line that already
// carries color bytes is left as it is.
std::string HighlightChatText(const std::string& text);

// A line without its "Ready Up: " lead-in: the chat prefix ("[Ready Up]") already says who speaks,
// and both read "[Ready Up] Ready Up: ..." (2026-11-03).
std::string WithoutReadyUpLeadIn(const std::string& text);

}  // namespace readyup
