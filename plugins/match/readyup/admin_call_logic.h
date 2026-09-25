#pragma once

// `.admin [message]` (admin_call.h), the engine-free half: the per-player cooldown, the message
// cleanup, the call id / timestamp and the `admin_called` payload (the webhook event and the
// fleet `event.admin_called` data). ctest `match_live_cards`.

#include "readyup/status_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace readyup {

// One call per player per `cooldownSeconds` (readyup.cfg admin_call_cooldown_s).
class AdminCallCooldown {
 public:
  // True when `steamid64` may call at `now` (seconds, any monotonic clock); the cooldown then
  // starts. False while it runs: *secondsLeft = whole seconds until the next call (at least 1).
  // cooldownSeconds <= 0: always true.
  bool TryCall(uint64_t steamid64, double now, int cooldownSeconds, int* secondsLeft = nullptr);
  void Clear() { last_.clear(); }

 private:
  std::unordered_map<uint64_t, double> last_;
};

constexpr size_t kAdminCallMessageMaxChars = 200;

// The player's text after `.admin`: control bytes (chat colors) dropped, tabs / newlines turned
// into spaces, runs of spaces collapsed, trimmed, cut to `maxChars` characters (UTF-8 code
// points; a sequence is never split). Empty is fine.
std::string CleanAdminCallMessage(const std::string& raw, size_t maxChars = kAdminCallMessageMaxChars);

// "2026-09-25T12:34:56.789Z" (ISO 8601, UTC, milliseconds).
std::string IsoUtcFromUnixMs(long long unixMs);

// A version-4 UUID string ("xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx", lowercase) from 16 random bytes.
std::string CallIdFromBytes(const unsigned char bytes[16]);

struct AdminCallPlayer {
  uint64_t steamid64 = 0;
  std::string name;
  std::string team;  // "team1" | "team2" | "spectator" | "" (= null: unknown / scrim player)
  std::string side;  // "ct" | "t" | "" (= null)
};

struct AdminCallEvent {
  bool hasMatch = false;  // false (scrim / no match loaded): matchid is null
  uint64_t matchid = 0;
  int map_number = 1;
  std::string call_id;
  AdminCallPlayer player;
  std::string message;    // CleanAdminCallMessage
  std::string called_at;  // IsoUtcFromUnixMs
};

// {call_id, player{steamid64, name, team, side}, message, called_at}: the fleet event's `data`.
status::Json AdminCallData(const AdminCallEvent& e);
// The webhook body: {"event":"admin_called","matchid":<n>|null,"map_number":<n>, ...AdminCallData}.
std::string AdminCalledWebhookJson(const AdminCallEvent& e);

// "Team A, CT" / "CT" / "spectator" / "" for chat and the admin card.
std::string AdminCallTeamLabel(const std::string& teamName, const std::string& side, bool spectator);

}  // namespace readyup
