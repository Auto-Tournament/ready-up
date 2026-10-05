// The platform commands fleet.so runs itself (FLEET.md §7.4), so a server without the match plugin
// (a practice server: core + fleet + practice) still takes them: plugins.set, whitelist.set,
// practice.set and say. Every other cmd goes to the plugins that registered for "cmd" (the match
// plugin); without one, fleet.so answers it as unsupported. No engine calls here: parsing and the
// cmd.result payload (fleet_plugin.cpp runs them).
#pragma once

#include "fleet_json.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fleet::cmds {

// plugins.set, whitelist.set, practice.set, say.
bool OwnedByFleet(const std::string& name);

// plugins.set {enable?: [name], disable?: [name]}: [a-z0-9_-]{1,32}, at most 16 each, not both
// lists, fleet cannot be disabled (the link runs in it).
bool ParsePluginsSet(const json::Value& args, std::vector<std::string>* enable, std::vector<std::string>* disable,
                     std::string* err);
// whitelist.set {enabled, steamids?: [SteamID64 string]} (at most 1000).
bool ParseWhitelistSet(const json::Value& args, bool* enabled, std::vector<uint64_t>* steamids, std::string* err);
// practice.set {on?: bool, always?: bool}: at least one. *on / *always: -1 = not given, else 0 / 1.
bool ParsePracticeSet(const json::Value& args, int* on, int* always, std::string* err);
// say: control characters dropped, at most 190 bytes (cut on a UTF-8 boundary).
std::string SanitizeSay(const std::string& text);

struct Result {
  std::string status = "ok";  // ok | rejected | failed | expired
  std::string code, message;
};
Result Ok();
Result Rejected(const std::string& code, const std::string& message);
Result Failed(const std::string& code, const std::string& message);
// The cmd.result payload: {status, error?: {code, message}, audit_id?}.
std::string ResultPayload(const Result& r, const std::string& auditId);

}  // namespace fleet::cmds
