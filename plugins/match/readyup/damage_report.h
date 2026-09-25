#pragma once

// End-of-round damage report (docs/PARITY.md §7, readyup.cfg / match.cfg `damage_report=1`, on
// by default). After every round of a live map each player on CT/T gets, privately (chat to
// their slot), one line per opponent:
//
//   To: 54 in 2 | From: 27 in 1 — Name (46 hp)
//
// from player_hurt (damage capped at the victim's remaining health) and player_death. Knife
// rounds and warmup are not reported. Bots get no chat; with readyup.cfg debug=1 every line is
// also logged (`[dbg] damage-report: <name>: <line>`), plus one `damage-report: round ...`
// summary line per round. The bookkeeping is damage_ledger.h.

#include "readyup/plugin_api.h"

namespace readyup {

// readyup_plugin_load: round_start, round_end, player_hurt, player_death.
void DamageReportInstall(const ru_api* api);
// on_tick: sends the reports built at round_end.
void DamageReportTick();
// A player damaged a player of the other CS team this round (.stop no_damage rule).
bool DamageReportEnemyDamageThisRound();

}  // namespace readyup
