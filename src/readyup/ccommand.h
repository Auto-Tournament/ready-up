#pragma once

#include <optional>
#include <string>
#include <vector>

namespace readyup {

// Reads the arguments of an engine CCommand using the engine-surface layout "CCommand"
// (argc/argv offsets), which is only trusted while ISource2GameClients::ClientCommand -- the
// engine code that reads exactly those offsets -- verified. Replaces the old "scan the object for
// pointers that look like strings" heuristic.
//
// nullopt if the layout is unverified or the object fails its sanity checks (argc outside
// 0..64, unreadable argv). Every read is fault-safe.
std::optional<std::vector<std::string>> ReadCCommandArgs(const void* cmd);

// True when the CCommand layout is verified (ReadCCommandArgs can succeed).
bool CCommandLayoutVerified();

// argv[1..] joined with single spaces (what a `say` command carries), or "" if unavailable.
std::string CCommandArgString(const std::vector<std::string>& args);

}  // namespace readyup
