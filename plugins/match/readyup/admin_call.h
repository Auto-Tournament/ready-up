#pragma once

// `.admin [message]`: any player calls an admin (docs/ADMINS.md "Calling an admin").
//
// - Per player once every admin_call_cooldown_s (readyup.cfg, default 60); on cooldown the caller
//   gets a private "you can call again in Ns".
// - The caller gets a private "admins notified".
// - Every in-game admin (IsReadyUpAdmin) gets a private chat line and, for a few seconds, a center
//   card at RU_HTML_PRIO_ALERT: "<name> (<team>) needs an admin" + the message.
// - The platform: the `admin_called` webhook event (webhook.cpp, the same events URL, auth and
//   envelope as the other events; payload: admin_call_logic.h AdminCalledWebhookJson), and on
//   the fleet link `event.admin_called` (plugins/fleet/protocol/v1/messages) while the server
//   has a platform assignment.
// - The console log: `admin-call: <name> (<steamid64>, <team>): <message> [call_id]`.
// Resolving a call is the platform's job; there is no in-game command for it.

#include <cstdint>
#include <string>

namespace readyup {

// The chat command (game thread). `text` is the whole line (".admin need help").
void AdminCallCommand(uint64_t steamid64, const std::string& playerName, const std::string& text);

// GameFrame thread: sends / refreshes the admin cards.
void AdminCallTick();

// Thread-safe: an admin card is up for this player slot (the ready HUD, the welcome and the
// go-live card leave the panel alone).
bool AdminCallCardActiveFor(int slot);

}  // namespace readyup
