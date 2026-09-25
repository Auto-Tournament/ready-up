#pragma once

#include <string>

namespace readyup {

// Starts a best-effort log observer and routes `ru ...` messages observed in chat.
// Safe to call multiple times.
void StartLogReceiver();

// Feeds one server log line to the plugins (subscribe_log_line) and into the log-derived
// lifecycle events (RU_EVENT_MAP_START / MATCH_START always; ROUND_START / ROUND_END while
// engine game events are not delivered). Called by the in-process logging listener for every
// line; the file reader only uses it when that listener is not installed.
void ObserveLifecycleLogLine(const std::string& line);

}  // namespace readyup
