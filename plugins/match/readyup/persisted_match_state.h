#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace readyup::persisted_match_state {

// Active match config JSON (raw MAT match config response body).
void PersistActiveMatchJson(std::string json);
void ClearActiveMatch();

// Indicates whether we ever transitioned to live for this active match.
void PersistLiveFlag(bool live);

// Persist a minimal runtime snapshot.
void PersistSnapshot(int map_number, int round_number, int team1_score, int team2_score);

// Crash recovery progress record (round_restore_rules.h Progress as JSON; match_recovery.h).
void PersistProgress(std::string json);
std::optional<std::string> GetProgressJson();

// Pre-match values of the match config's cvars (cvar_snapshot.h), "" = none. Kept apart from the
// per-match keys: a second match loaded over the first keeps the original values.
void PersistCvarSnapshot(std::string json);
std::optional<std::string> GetCvarSnapshotJson();

// Persist current backup prefix and last-known backup file name.
void PersistBackupPrefix(std::string prefix);
void PersistBackupFile(std::string filename);

// Read helpers (best-effort).
std::optional<std::string> GetActiveMatchJson();
bool GetLiveFlag();
std::optional<std::string> GetBackupFile();
std::optional<int> GetLastRoundNumber();

}  // namespace readyup::persisted_match_state

