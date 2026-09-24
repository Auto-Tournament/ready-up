#pragma once

#include <string>

namespace readyup {

// Starts a best-effort log observer and routes `ru ...` messages observed in chat.
// Safe to call multiple times.
void StartLogReceiver();

// Feeds one server log line into the log-derived match lifecycle tracker
// (map changes, Match_Start, Round_Start -> OnMatchRoundStarted, round-end
// scores, kills, connect/disconnect webhooks). Called by the in-process
// logging listener for every line; the file reader only uses it when that
// listener is not installed. No-ops for round lifecycle when engine game
// events are installed (events drive it then).
void ObserveLifecycleLogLine(const std::string& line);

}  // namespace readyup
