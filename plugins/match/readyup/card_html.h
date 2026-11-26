#pragma once

// One-off cards of the match plugin as clickable-HUD screens (spec lines), engine-free (ctest
// `match_live_cards`): the go-live card (golive_card.h), the map / series result (result_card.h)
// and the admin-call card (admin_call.h). The old centre-HTML versions are gone (2026-11-26): with
// no HUD these are chat lines.

#include <cstddef>
#include <string>

// Every card takes the viewer's language (`lang`, locales/ keys card.*); "en" by default.

namespace readyup {

struct GoLiveCardInfo {
  std::string team1, team2;  // team names; both empty (scrim): no matchup line
  int team1Side = 0;         // 3 = CT, 2 = T, 0 = unknown (no side shown)
  bool pauses = false;       // .p / .pause / .tech, .up / .unpause, .tac work (a match is loaded)
  bool adminCall = true;     // .admin
};
// The go-live commands for chat, short forms only: "Pause: .p | Unpause: .up | Timeout: .tac |
// Call an admin: .admin". Empty when there is nothing to show.
std::string GoLiveCommandsLine(const GoLiveCardInfo& info, const std::string& lang = "en");

// The result cards (result_card.h), shown to everyone at the end of a map and of the series.
struct ResultCardInfo {
  std::string team1, team2;      // names; empty: "Team 1" / "Team 2"
  std::string winner;            // "team1" | "team2" | "" (a draw)
  int team1Score = 0, team2Score = 0;  // rounds (map card) or maps (series card)
  int mapNumber = 0;             // map card: 1-based
  std::string mapName;           // map card: "de_mirage" -> "Mirage"
  std::string nextMap;           // map card: the next map, if any
  bool forfeit = false;          // series card: won by forfeit
  bool decidedByDamage = false;  // the (last) map was tied after its overtimes and won on damage
  int team1Damage = 0, team2Damage = 0;
};
// The cards as clickable-HUD screens (spec lines, hud_screens.h / plugins/hud/hud_screen.h).
std::string GoLiveCardSpec(const GoLiveCardInfo& info, const std::string& lang = "en");
std::string MapResultCardSpec(const ResultCardInfo& info, const std::string& lang = "en");
std::string SeriesResultCardSpec(const ResultCardInfo& info, const std::string& lang = "en");
std::string AdminCallCardSpec(const std::string& name, const std::string& teamLabel, const std::string& message,
                              const std::string& lang = "en");

}  // namespace readyup
