#pragma once

// Is this CS2 server behind Steam's current build? Pure parts (unit tested by ctest
// `match_cs2_update`): the number Steam's UpToDateCheck wants, its answer, and the "report each
// required version once" rule. The fetch runs on a worker thread from fleet_bridge.cpp (the
// platform gets `server.cs2_update_required {required_build}`, docs/FLEET.md §8.1, §14.3).

#include <string>

namespace readyup::cs2update {

constexpr int kAppId = 730;

// "1.40.3.2" / "Patchversion 1.40.3.2" -> 14032 (the digits of the first dotted number, what
// ISteamApps/UpToDateCheck calls `version`). -1 when there is none.
long long PatchVersionNumber(const std::string& text);

struct Answer {
  bool ok = false;         // a well-formed success answer
  bool upToDate = true;
  long long required = 0;  // required_version, when not up to date
};

// The body of ISteamApps/UpToDateCheck/v1 ({"response":{"success":true,"up_to_date":false,
// "required_version":14035,...}}).
Answer ParseAnswer(const std::string& body);

std::string CheckUrl(long long currentVersion);

// True when `answer` says the server is behind and that required version was not reported yet
// (`lastReported`, 0 = none).
bool ShouldReport(const Answer& answer, long long lastReported);

}  // namespace readyup::cs2update
