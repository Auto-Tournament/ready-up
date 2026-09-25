#pragma once

// One-off center cards of the match plugin, engine-free (ctest `match_live_cards`):
// - the go-live card (golive_card.h): "LIVE · GO GO GO", the teams and their sides, the
//   commands that work now,
// - the admin-call card (admin_call.h) shown to in-game admins.
// What renders in the panel: docs/HUD.md. Both stay well under 1 KB.

#include <cstddef>
#include <string>

namespace readyup {

// HTML-escapes `in` for the center panel: < > & " ' become entities, control bytes (chat colors)
// are dropped. Longer than maxBytes: cut there (never inside a UTF-8 sequence) and "..." added.
std::string CardHtmlEscape(const std::string& in, size_t maxBytes);

struct GoLiveCardInfo {
  std::string team1, team2;  // team names; both empty (scrim): no matchup line
  int team1Side = 0;         // 3 = CT, 2 = T, 0 = unknown (no side shown)
  bool pauses = false;       // .p / .pause / .tech, .up / .unpause, .tac work (a match is loaded)
  bool adminCall = true;     // .admin
};
std::string GoLiveCardHtml(const GoLiveCardInfo& info);

// "<name> (<teamLabel>) needs an admin" + the message (if any), for admins.
std::string AdminCallCardHtml(const std::string& name, const std::string& teamLabel, const std::string& message);

}  // namespace readyup
