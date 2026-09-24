#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace readyup {

// Minimal team identity for MAT/MatchZy-style events.
enum class WebhookTeam {
  Unknown = 0,
  Team1,
  Team2,
};

struct WebhookPlayer {
  uint64_t steamid64 = 0;
  std::string name;
  WebhookTeam team = WebhookTeam::Unknown;
};

struct WebhookPlayerStatLine {
  uint64_t steamid64 = 0;
  std::string name;
  WebhookTeam team = WebhookTeam::Unknown;
  int kills = 0;
  int deaths = 0;
};

// MatchZy-style per-player stats line (nested under team1/team2).
struct WebhookPlayerStats {
  uint64_t steamid64 = 0;
  std::string name;
  int kills = 0;
  int deaths = 0;
  int assists = 0;
  int headshot_kills = 0;
  int damage = 0;
  int mvps = 0;
  int score = 0;
};

struct WebhookMatchContext {
  uint64_t matchid = 0;
  std::string slug;  // optional
  int num_maps = 0;
  std::string team1_name;
  std::string team2_name;

  uint64_t team1_captain_steamid64 = 0;
  uint64_t team2_captain_steamid64 = 0;

  // Knife decider decision window (seconds). If the knife-winning team does not
  // pick sides in time, sides stay.
  int knifeDecisionSeconds = 60;

  // Regulation total rounds (e.g. 24 for MR12).
  // This is sourced from MAT config `maxRounds` (preferred) or `cvars.mp_maxrounds` (fallback).
  int maxRounds = 24;

  // Overtime settings (sourced from MAT config metadata, not convars).
  bool overtime_enabled = false;
  // Rounds per overtime half (e.g. 3 => MR3 halves, 6 rounds per OT).
  int overtimeSegments = 3;

  // Optional tie-break configuration.
  // - maxOvertimes: maximum number of overtime blocks (each block is 2*overtimeSegments rounds).
  //   -1 = unlimited (default/current behavior).
  int maxOvertimes = -1;
  // When enabled, ReadyUp can resolve ties using total roster-team damage.
  bool damageTiebreakEnabled = false;
  // If damage is also tied, continue playing sudden-death rounds until a winner.
  bool suddenDeathOnDamageTie = true;

  // Side mapping per map index (1-based map_number -> index map_number-1).
  // Values: "team1_ct" | "team2_ct" | "knife" (and possibly others).
  std::vector<std::string> map_sides;

  // Map list for the series (map 1 is maplist[0]).
  // This is sourced from MAT match config.
  std::vector<std::string> maplist;

  // SteamID64s allowed to join as spectators (whitelist).
  std::unordered_set<uint64_t> spectators;

  // SteamID64s treated as admins for this match context.
  // This is sourced from MAT match config `admins: [steamid64...]`.
  std::unordered_set<uint64_t> admins;

  // Match config cvars (e.g. mp_maxrounds, mp_overtime_enable, matchzy_*).
  // Values are stored as raw strings and applied as: `<key> <value>`.
  std::unordered_map<std::string, std::string> cvars;

  // SteamID64 -> team mapping derived from match config.
  std::unordered_map<uint64_t, WebhookTeam> roster_team;
};

// Configure base events URL (e.g. https://mat.example.com/api/events).
// Empty string disables webhook sending.
void WebhookConfigure(std::string baseEventsUrl);

std::string WebhookBaseUrl();

// Configure the MAT heartbeat URL (full URL including /api/servers/:id/heartbeat).
// Empty string disables heartbeats.
void WebhookConfigureHeartbeatUrl(std::string heartbeatUrl);
std::string WebhookHeartbeatUrl();

// Sets optional Bearer token for webhook requests.
// This is expected to be the same token used for `ru match load` auth.
void WebhookSetBearerToken(std::optional<std::string> token);

// Set/clear match context (called when we load match config).
void WebhookSetMatchContext(WebhookMatchContext ctx);
void WebhookClearMatchContext();
std::optional<WebhookMatchContext> WebhookGetMatchContext();

// Override heartbeat status even when no match is loaded.
// Accepted values: "idle"|"loading"|"warmup"|"live"|"postgame"|"error"
// (Any other value clears the override to "idle" semantics.)
void WebhookSetHeartbeatStatus(const char* status);

// Enqueue an event JSON payload. This is non-blocking best-effort.
// The payload must already be valid JSON.
void WebhookEnqueueEvent(std::string json);

// Convenience helpers for core events.
void WebhookEmitServerConfigured(const char* configuredBy);
void WebhookEmitServerHealth(const char* reason = nullptr);
void WebhookEmitSeriesStart();
void WebhookEmitSeriesEnd(int team1_series_score, int team2_series_score, const char* winner, int time_until_restore);
void WebhookEmitMapResult(int map_number, const char* map_name, int team1_score, int team2_score, const char* winner);
void WebhookEmitPlayerConnect(const WebhookPlayer& p);
void WebhookEmitPlayerDisconnect(const WebhookPlayer& p);
// Emits MatchZy-style ready/unready events with counts.
// - If ready==true: emits `player_ready`
// - If ready==false: emits `player_unready`
void WebhookEmitPlayerReady(const WebhookPlayer& p,
                            bool ready,
                            int ready_count_team1,
                            int ready_count_team2,
                            int total_ready,
                            int expected_total);
void WebhookEmitMatchPaused(int map_number, const WebhookPlayer& paused_by, bool is_tactical, bool is_admin, int pause_time);
void WebhookEmitUnpauseRequested(int map_number, WebhookTeam team, int teams_ready, int teams_needed);
void WebhookEmitMatchUnpaused(int map_number, int pause_duration);
void WebhookEmitWarmupEnded(int map_number);
void WebhookEmitGoingLive(int map_number);
void WebhookEmitHalftimeStarted(int map_number, int team1_score, int team2_score);
void WebhookEmitOvertimeStarted(int map_number, int overtime_number);
void WebhookEmitSideSwap(int map_number, const char* team1_side, const char* team2_side);
void WebhookEmitKnifeRoundStarted(int map_number);
void WebhookEmitKnifeRoundEnded(int map_number, const char* winner);
// MatchZy-style side pick event (typically after knife).
// - side: "ct"|"t" (lowercase MatchZy convention)
// - picked_by: player name or "server"
// - team: "team1"|"team2"
void WebhookEmitSidePicked(int map_number, const char* map_name, const char* side, const char* picked_by, const char* team);
// Thread-safe match context mutation: replaces map_sides[map_number-1] with "team1_ct" or "team2_ct".
bool WebhookUpdateMapSide(int map_number, const char* map_side);
// Admin-triggered recovery request. This tells MAT to rebuild its UI state from DB events,
// optionally rewinding to a specific round number.
// - round_number == 0 => recover to latest
void WebhookEmitRecoverRequested(int map_number, int round_number);
void WebhookEmitRoundStarted(int map_number, int round_number, int team1_score, int team2_score);
void WebhookEmitRoundEnd(int map_number, int round_number, int round_time, int reason, const char* winner, int team1_score, int team2_score);
void WebhookEmitRoundEndWithPlayerStats(int map_number,
                                        int round_number,
                                        int round_time,
                                        int reason,
                                        const char* winner,
                                        int team1_score,
                                        int team2_score,
                                        const std::vector<WebhookPlayerStatLine>& players);

// Preferred: MatchZy-compatible nested shape so MAT can parse it.
void WebhookEmitRoundEndMatchzy(int map_number,
                                int round_number,
                                int round_time,
                                int reason,
                                const char* winner,
                                int team1_score,
                                int team2_score,
                                const std::vector<WebhookPlayerStats>& team1_players,
                                const std::vector<WebhookPlayerStats>& team2_players);

// Starts sender thread (idempotent). Safe to call early during startup.
void WebhookStartSenderThread();

}  // namespace readyup

