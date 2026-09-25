#pragma once

// readyup-match settings.
//
// Read from, in order (later wins):
//   1. readyup.cfg next to the core, top-level keys (where these keys lived while the match flow
//      was part of the core, so existing configs keep working),
//   2. the `[match]` section of readyup.cfg,
//   3. csgo/cfg/ReadyUp/match.cfg (top-level keys or a `[match]` section).
// Re-read automatically when any of those files changes (checked once a second), and on
// `ru reload`. Environment overrides as before: READYUP_DEV_BOTS_READY, READYUP_DEV_BOTS_SCRIM,
// READYUP_CHAT_DEBUG. `debug` is the core's (ru_api debug_enabled).

#include "readyup/match_rules.h"

#include <string>

namespace readyup {

struct ReadyUpCfg {
  // Per-player center-HTML welcome screen on first T/CT join each map.
  bool welcome = true;
  // Per-player center-HTML ready list during scrim/match warmup + knife side pick.
  bool ready_hud = true;
  // Center-panel refresh tuning (ms / s).
  int hud_tick_ms = 0;
  int hud_resend_ms = 0;
  int hud_duration_s = 1;
  // Seconds the KNIFE ROUND panel stays up after the knife round begins.
  int hud_knife_hold_s = 30;
  // Header of the welcome card + ready HUD: optional logo image URL + brand text.
  std::string hud_brand = "Auto Tournament";
  std::string hud_logo_url;
  bool chat_debug = false;
  // Admin / captain chat prefixes (token-based; expanded to CS2 chat control bytes).
  std::string admin_prefix;
  std::string captain_prefix_team1;
  std::string captain_prefix_team2;
  // Hide `.r` / `.ready` / `.ur` ... from chat (RU_CMD_HIDE).
  bool consume_ready_chat = false;
  // DEBUG ONLY: bots on CT/T count toward the scrim roster and are always READY.
  bool dev_bots_ready = false;
  // DEBUG ONLY: a scrim can start and run with only bots on CT and T.
  bool dev_bots_scrim = false;
  // Scrims: knife round after everyone readied up.
  bool scrim_knife = true;
  // Scrim / match warmup: money topped up to mp_maxmoney after every purchase (warmup_money.h),
  // like CS2's own warmup. 0 = off (players keep what they did not spend).
  bool warmup_money = true;
  // Scrim knife side-pick window in seconds (match configs use knifeDecisionSeconds).
  int knife_pick_seconds = 60;
  // Damage report in chat to each player after every live round (damage_report.h).
  bool damage_report = true;
  // Idle map refresh (idle_refresh.h): reload the map after this many hours on it with no match
  // loaded and nobody connected. 0 = off.
  int idle_map_refresh_hours = 12;
  // Pause / ready / forfeit rules (match_rules.h): the values for scrims, and the fallback for
  // match configs that leave a rule out. Keys: max_tech_pauses_per_team, tech_pause_max_seconds,
  // both_teams_unpause_required, allow_force_ready, min_players_to_ready, forfeit_after_seconds.
  MatchRules rules;
  // Ruleset for loaded matches (ruleset.h, docs/ESPORTS-MODE.md): "default" | "valve". A match
  // config's "ruleset" wins. Anything else is logged and read as "default".
  std::string ruleset = "default";
  // Models the `default_models` rule resets players to (per team, the map-independent defaults).
  std::string default_model_ct = "agents/models/ctm_sas/ctm_sas.vmdl";
  std::string default_model_t = "agents/models/tm_phoenix/tm_phoenix.vmdl";
};

ReadyUpCfg Cfg();
// ru_api debug_enabled (readyup.cfg debug=1 or READYUP_DEBUG=1). Any thread.
bool DebugEnabled();
bool ChatDebugEnabled();
std::string AdminPrefix();
std::string CaptainPrefixTeam1();
std::string CaptainPrefixTeam2();
bool ConsumeReadyChat();
// Log a loud line whenever the effective value flips.
bool DevBotsReadyEnabled();
bool DevBotsScrimEnabled();
// `ru_dev_bots_scrim 0|1|cfg`: -1 = no override (cfg/env), 0 = off, 1 = on.
void SetDevBotsScrimOverride(int v);
int DevBotsScrimOverride();

// Re-reads every source now. False (and *err) if readyup.cfg could not be read.
bool ReloadCfg(std::string* err = nullptr);
// Frame: re-reads when a source file changed (stat at most once a second). True if it did.
bool MaybeReloadCfg();

}  // namespace readyup
