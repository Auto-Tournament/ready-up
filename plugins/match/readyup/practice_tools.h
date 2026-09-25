#pragma once

// Practice-mode tools (docs/PARITY.md §7 "Practice extras"). Only in practice mode (`.prac`),
// which execs ReadyUp/prac.cfg (sv_cheats true). CS2's own cheat commands where they work from
// the server console, schema reads / writes of the player's pawn, and one engine call for moves.
//
// CS2 1.41.8.4: setpos, setpos_player, setang, ent_setpos, ent_fire and bot_place need a client
// of their own (the handler bails out when the command has no issuing player), so from the server
// console they do nothing. Moves therefore use ru_api entity_set_abs_origin (v1.3, engine surface
// CBaseEntity_SetAbsOrigin: the call setpos / ent_setpos make). View angles cannot be set for
// another player (no per-player setang), so they are never restored.
//
//   .rethrow / .rt        sv_rethrow_last_grenade: the last grenade thrown on the SERVER (CS2 has
//                         no per-player variant; with several players it rethrows whoever threw last).
//   .savepos [name]       remember your position (pawn scene node origin), per map
//   .loadpos [name]       move back to it
//   .back                 back to where you threw your last grenade (grenade_thrown), else to
//                         where you were before your last .loadpos / .spawn
//   .clear                ent_remove_all smoke / molotov / decoy projectiles and fires (inferno)
//   .noflash              toggle: your pawn's m_flFlashMaxAlpha 0 (+ m_flFlashDuration 0 on player_blind)
//   .god                  toggle: your pawn's m_bTakesDamage (prac.cfg's buddha already keeps
//                         everyone alive; .god also keeps your hp). Falls back to toggling buddha.
//   .bot .cbot .boost ... a bot on the other team, moved to where you stand once it spawns.
//                         bot_place is not used (console: "could not find a human player").
//   .nobots               bot_kick
//   .spawn N / .ctspawn N / .tspawn N   to spawn point N (1-based) of your / the CT / the T team:
//                         info_player_counterterrorist / info_player_terrorist entities (enabled
//                         ones, by priority then entity index; server-only entities, index > 16384).
//
// Test hook: `ru as <slot> <chat line>` (server console / RCON only) runs a practice command,
// `.gg` or `.stop` as the player (or bot) in that slot, so bot-only live tests can drive them.
// Under the valve ruleset the tools above do nothing (ruleset.h PlayerExtrasAllowed); the bot
// commands keep working as before this change.
// Log lines: `practice: ...`.

#include "readyup/plugin_api.h"

#include <cstdint>
#include <string>
#include <vector>

namespace readyup {

// readyup_plugin_load: chat commands, `ru as`, grenade_thrown / player_blind.
void PracticeToolsInstall(const ru_api* api);
// on_tick: pending bot placements; forgets toggles / positions when practice ends.
void PracticeToolsTick(double now);
// `.bot`, `.cbot`, `.crouchbot`, `.boost`, `.crouchboost`, `.nobots` (match_router.cpp).
void PracticeToolsBotCommand(int slot, uint64_t id, const std::string& cmd);
// Practice help line for `.help`.
const char* PracticeToolsHelp();

}  // namespace readyup
