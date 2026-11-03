#pragma once

// The platform's full-team timer and walkover choice (fleet rules.full_team; webhook.h
// WebhookMatchContext full_team_* / walkover_*). The platform holds the match while it is open.
//
// - Before the deadline, a team that is not full sees the countdown on its warmup card.
// - Once the time is up with one team short, the choice replaces the ready card for everyone:
//   the full team's owner and captains get "Take the walkover" (.walkover) / "Play them short"
//   (.play) with the cursor on; their teammates, the short team and spectators see who chooses.
// - `.walkover` / `.play` from a chooser: `event.walkover_choice` on the fleet link. The platform
//   decides, answers in chat and sends the match again (the choice is gone, or the match ends).

#include "readyup/webhook.h"

#include <cstdint>
#include <string>

namespace readyup {

// `.walkover` / `.play` (game thread).
void WalkoverCommand(uint64_t steamid64, const std::string& playerName, bool take, int slot);

// The choice screen for this viewer, or "" when none is open (hud_screens.h spec).
std::string WalkoverHudSpec(const WebhookMatchContext& ctx, uint64_t viewer);

// Seconds left to have a full team on the server, or -1 (no timer, or past it).
long long FullTeamSecondsLeft(const WebhookMatchContext& ctx);

}  // namespace readyup
