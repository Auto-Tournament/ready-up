#pragma once

// Wingman (match config `wingman: true`, fleet `rules.wingman`): 2v2 on CS2's wingman game mode.
// The pure part (ctest `match_simulation`); the match flow uses it in match_config_parser.cpp
// (defaults), modes.cpp (game mode before the map load) and esports.cpp (the go-live cfg).
//
// CS2 reads game_type / game_mode when a map loads, so a match load sets them right before its
// changelevel: classic competitive (0 / 1) or wingman (0 / 2). The go-live cfg is
// ReadyUp/live_wingman.cfg (MR8, 2v2 economy, MR2 overtime); it runs even with
// ru_cfg_exec_enable 0, like the valve ruleset's cfg, because a wingman map without it would
// play competitive rules. Wingman and the valve ruleset (5v5 CS Major rules) do not go together.

#include <string>
#include <vector>

namespace readyup {
namespace wingman {

constexpr const char* kLiveCfg = "ReadyUp/live_wingman.cfg";
// Regulation rounds (MR8) and overtime rounds per half (mp_overtime_maxrounds 4) when the match
// config sets neither maxRounds / mp_maxrounds nor overtimeSegments.
constexpr int kMaxRounds = 16;
constexpr int kOvertimeHalf = 2;
// Players per side (simulation fills a team without a roster with this many bots).
constexpr int kPlayersPerTeam = 2;

// game_type / game_mode for a match: {"game_type 0", "game_mode 2"} (wingman) or
// {"game_type 0", "game_mode 1"} (competitive). They take effect on the next map load.
std::vector<std::string> GameModeCommands(bool wingman);

}  // namespace wingman
}  // namespace readyup
