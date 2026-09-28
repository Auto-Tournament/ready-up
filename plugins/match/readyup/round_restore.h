#pragma once

// Round restore commands and what follows every restore (docs/PARITY.md §7). Pure logic:
// round_restore_rules.h.
//
//   .restore <round>             admin chat: play <round> of the current map again from its start
//   .ru match restore <round>    the same (also `ru match restore <round>` on the console / RCON)
//   .ru match backups            the loaded match's round backups on this server
//   ru_listbackups [matchid]     console: the same list (the Auto Tournament CS2 name)
//   ru_loadbackup <file>         console: restore that backup file of the loaded match
//   ru_pause_after_restore 0|1   console setting (state.json): stay paused after a restore (1,
//                                default) or go live 3 s later; a match config's
//                                `pause_after_restore` (MAT key, fleet rules.pause) wins;
//                                under the valve ruleset a restore always stays paused
//
// A restore loads CS2's own backup of the round start (mp_backup_restore_load_file, see
// fleet_bridge.h RestoreRoundFromLocalBackup) and voids the rounds from there in Ready Up's round
// counters and stats. After an admin restore the match waits paused like after a technical pause
// (both teams .unpause; admins .fup). Every restore (admin, `.stop` vote, fleet cmd restore_round,
// a failover resume, crash recovery) sends the `backup_loaded` webhook.
//
// Log lines: `restore: ...`.

#include "readyup/round_restore_rules.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace readyup::round_restore {

using Reply = std::function<void(const std::string&)>;

// `.ru match restore <round>` (admin checked by the caller). by: who asked (name / "Console").
// reply: an answer to the sender; announce: a line for everyone. Game thread.
void RestoreCommand(const std::vector<std::string>& args, const std::string& by, const Reply& reply,
                    const Reply& announce);
// `.ru match backups`.
void BackupsCommand(const Reply& reply);

// Console lines ru_pause_after_restore / ru_listbackups / ru_loadbackup; false when not ours.
bool HandleConsoleLine(const std::string& line);
// The console setting: -1 = not set (readyup.cfg / default), 0 / 1.
int ConsolePauseAfterRestore();
void SetConsolePauseAfterRestore(int v);
// Effective for the loaded match (round_restore_rules.h PauseAfterRestoreFor).
bool PauseAfterRestore();

// fleet_bridge DoRestore, after every restore: backup_loaded webhook, progress for crash recovery
// and, unless `reason` is "resume" / "recovery" (they unpause themselves), the automatic unpause
// 3 s later when pause_after_restore is off. True when that unpause was scheduled. Game thread.
bool AfterRestore(int mapNumber, int round, const std::string& file, const std::string& reason);

// The round backups of match `matchid` in CS2's backup directories (fleet_bridge.cpp
// BackupDirs), sorted (round_restore_rules.h SortBackups). Any thread.
std::vector<restore::BackupInfo> ListBackups(uint64_t matchid);

// Going live on a map: CS2 round backups on, named for this match and map (a restart of the
// server or the next map would otherwise keep the previous map's name).
void EnableRoundBackups(uint64_t matchid, int mapNumber);

}  // namespace readyup::round_restore
