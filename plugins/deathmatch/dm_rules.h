#pragma once

// readyup-deathmatch pure logic (ctest `deathmatch_rules`): settings, the cvars of each mode,
// scoring, win conditions, the leaderboard panel (ordering, HTML escaping), weapon rounds and which
// map `.ru dm ffa|tdm [map]` loads. No engine calls.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace deathmatch {

enum class Mode { Off = 0, Ffa = 1, Tdm = 2 };
const char* ModeName(Mode m);   // "off" | "ffa" | "tdm" (also the essentials default_maps key)
const char* ModeLabel(Mode m);  // "Deathmatch" | "Team deathmatch"
bool ParseMode(const std::string& s, Mode* out);  // ffa / tdm (case-insensitive); "dm" = ffa

// ---- settings (cfg/ReadyUp/deathmatch.cfg or readyup.cfg [deathmatch]) --------------------------

struct Settings {
  int killLimitFfa = 30;           // kill_limit_ffa: first player to N kills wins (0 = no limit)
  int killLimitTdm = 100;          // kill_limit_tdm: first team to N kills wins (0 = no limit)
  int timeLimitMinutes = 10;       // time_limit_minutes: then the leader wins (0 = no limit)
  int spawnProtectionSeconds = 2;  // spawn_protection_seconds -> mp_respawn_immunitytime
  bool headshotOnly = false;       // headshot_only -> mp_damage_headshot_only
  bool hud = true;                 // hud: leaderboard panel
  int hudIntervalMs = 1000;        // hud_interval_ms: panel resend (0 = every tick)
  int endDelaySeconds = 10;        // end_delay_seconds: winner card, then the next game
  bool restartReloadsMap = false;  // restart = restartgame (default) | reload (load the map again)
  std::vector<std::string> weaponRounds;  // weapon_rounds = weapon_deagle, weapon_awp, ...
  int weaponRoundEveryMinutes = 0;        // weapon_round_every_minutes (0 = no weapon rounds)
  int weaponRoundSeconds = 60;            // weapon_round_seconds
  bool weaponRoundRestrictBuy = false;    // weapon_round_restrict_buy -> mp_buy_allow_guns 0
  int restoreGameType = 0;         // restore_game_type: `dm off` goes back to this game_type ...
  int restoreGameMode = 1;         // restore_game_mode: ... and game_mode (0 / 1: competitive)
};

// key -> value (true when set). The plugin wraps ru_api config_get.
using ConfigLookup = std::function<bool(const char* key, std::string* value)>;
// Reads every key; bad values keep the default and add a line to *warnings (if not NULL).
Settings LoadSettings(const ConfigLookup& get, std::vector<std::string>* warnings);
int KillLimit(const Settings& s, Mode m);

// ---- cvars ----------------------------------------------------------------------------------------

// CS2's deathmatch game mode. game_type / game_mode take effect on the next map load.
std::vector<std::string> GameModeCommands();  // game_type 1, game_mode 2
// The mode's rules on top of gamemode_deathmatch.cfg: sent at every map start (and once more a
// few seconds later) while the mode is on.
std::vector<std::string> ModeCommands(Mode m, const Settings& s);
// What `dm off` puts back (the next map load resets the rest: gamemode_competitive.cfg).
// withGameMode: also game_type / game_mode = restore_game_type / restore_game_mode.
std::vector<std::string> LeaveCommands(const Settings& s, bool withGameMode);

// weapon_[a-z0-9_]+ (fits a command line).
bool ValidWeapon(const std::string& w);
bool IsPistol(const std::string& w);
// Default-loadout cvars that hand out `weapon` on every spawn ("" = back to the normal loadout).
std::vector<std::string> WeaponRoundCommands(const std::string& weapon, bool restrictBuy);

// "weapon_deagle" -> "Desert Eagle" for the common ones, else the name without "weapon_".
std::string WeaponLabel(const std::string& weapon);

struct WeaponRound {
  int index = -1;       // into Settings::weaponRounds, -1 = no weapon round now
  std::string weapon;
  int secondsLeft = 0;
};
// Every weapon_round_every_minutes (first one after that many minutes) the next weapon of the
// list, for weapon_round_seconds (at most the whole period).
WeaponRound WeaponRoundAt(const Settings& s, double elapsedSeconds);

// ---- scoring --------------------------------------------------------------------------------------

struct PlayerScore {
  uint64_t id = 0;  // SteamID64, or BotId(userid) for bots
  std::string name;
  int team = 0;  // 2 = T, 3 = CT
  int kills = 0;
  int deaths = 0;
  int headshots = 0;
  double reachedAt = 0;  // when the current kill count was reached (earlier ranks higher)
};

// Bots have no SteamID: a pseudo id from their slot / userid (never a real SteamID64).
uint64_t BotId(int userid);

class Scoreboard {
 public:
  void Reset();
  // A player seen (connected / renamed / changed team). Keeps their score.
  void SeePlayer(uint64_t id, const std::string& name, int team);
  // One player_death. FFA: any kill of another player counts for the attacker. TDM: only kills of
  // the other team count, for the attacker and their team. Suicides, world and team kills only
  // add the victim's death. Returns true when a kill was counted.
  bool OnKill(Mode m, uint64_t attacker, int attackerTeam, uint64_t victim, int victimTeam, bool headshot, double now);
  // Kills desc, then deaths asc, then who reached their kills first, then name.
  std::vector<PlayerScore> Ranked() const;
  int RankOf(uint64_t id) const;  // 1-based, 0 = not on the board
  const PlayerScore* Find(uint64_t id) const;
  int TeamKills(int team) const;
  bool Empty() const { return players_.empty(); }

  // For `ru plugin reload deathmatch` (stash): one line per player.
  std::string Serialize() const;
  bool Deserialize(const std::string& text);

 private:
  PlayerScore* Get(uint64_t id);
  std::vector<PlayerScore> players_;
  int teamKills_[4] = {0, 0, 0, 0};
};

struct Outcome {
  bool over = false;
  bool draw = false;
  std::string reason;  // "kill_limit" | "time_limit"
  std::string winnerName;  // player (FFA) or "Counter-Terrorists" / "Terrorists" (TDM)
  uint64_t winnerId = 0;   // FFA
  int winnerTeam = 0;      // TDM
  int winnerKills = 0;
  int runnerUpKills = 0;
};
// Kill limit first (FFA: the top player at the limit; TDM: a team at the limit), then the time
// limit (the leader wins; equal kills at the top = draw).
Outcome CheckEnd(Mode m, const Scoreboard& b, const Settings& s, double elapsedSeconds);

// ---- output ---------------------------------------------------------------------------------------

// Numeric entities for & < > " ' (the center panel renders no named ones), control bytes
// dropped, cut to maxChars characters (UTF-8 aware) with "..." added.
std::string HtmlEscape(const std::string& s, size_t maxChars);

// The small leaderboard: title line (mode, limit, time left, weapon round), TDM team score, the top
// 5, and the viewer's own rank when not in the top 5. secondsLeft < 0 = no time limit.
std::string LeaderboardHtml(Mode m, const Scoreboard& b, uint64_t viewer, int killLimit, int secondsLeft,
                            const std::string& weaponRound);
std::string WinnerHtml(Mode m, const Outcome& o, int nextGameInSeconds);
std::string WinnerChat(Mode m, const Outcome& o);
std::string FormatClock(int seconds);  // "7:05"

// ---- maps -----------------------------------------------------------------------------------------

// A pasted Steam Workshop link becomes its id; anything else is returned as it is.
std::string MapArgToEntry(const std::string& arg);

struct MapChoice {
  std::string entry;   // what to load; "" = nothing loadable
  std::string source;  // "argument" | "default" | "current"
};
// `.ru dm ffa|tdm [map]`: the map given, else the mode's default map (essentials default_maps),
// else the current map again (reloadEntry: mapnames::ReloadEntry of the current map).
MapChoice ResolveMap(const std::string& arg, const std::string& defaultEntry, const std::string& reloadEntry);

}  // namespace deathmatch
