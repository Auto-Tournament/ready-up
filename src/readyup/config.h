#pragma once

 #include <string>

namespace readyup {

struct ReadyUpCfg {
  bool debug = false;
  bool banner = true;
  // Per-player center-HTML welcome screen on first T/CT join each map.
  bool welcome = true;
  // Per-player center-HTML ready list during scrim/match warmup + knife side pick.
  bool ready_hud = true;
  // Center-panel refresh tuning (ms / s). CS2 replays the panel's open
  // animation on every send, so these are live-tunable via `.ru reload`.
  int hud_tick_ms = 0;
  int hud_resend_ms = 0;
  int hud_duration_s = 1;
  // Header of the welcome card + ready HUD: optional logo image (URL, empty =
  // no image) followed by the brand text.
  std::string hud_brand = "Auto Tournament";
  std::string hud_logo_url;
  bool chat_debug = false;

  // UDP port ReadyUp listens on for server log lines (used to observe chat `ru ...` commands).
  // Must match `logaddress_add 127.0.0.1:<port>` in cfg.
  int log_receiver_port = 35050;

  // Prefixes (token-based; expanded to CS2 chat control bytes at load).
  // Note: ReadyUp will add a single space after these prefixes in output.
  std::string chat_prefix;
  std::string admin_prefix;

  // Captain prefixes (per team; optional; expanded like chat_prefix).
  // Used for "true-prefix" behavior (temporary name swap) in chat.
  std::string captain_prefix_team1;
  std::string captain_prefix_team2;

  // If true, hide `.ru ...` / `.r` messages from chat after processing.
  bool consume_ru_chat = false;
  bool consume_ready_chat = false;

  // DEBUG ONLY. When true, bots on CT/T count toward the pre-match scrim roster
  // and are always READY, so one human can test ready -> live with bots.
  // Bots are never added to the match context (no webhooks/DB/persistence).
  bool dev_bots_ready = false;

  // Scrims (no match config): play a knife round after everyone readied up;
  // the winning side picks .stay/.switch. Off: straight to live.
  bool scrim_knife = true;
  // Scrim knife side-pick window in seconds (match configs use knifeDecisionSeconds).
  int knife_pick_seconds = 60;
};

ReadyUpCfg Cfg();

bool DebugEnabled();
bool BannerEnabled();
bool ChatDebugEnabled();
int LogReceiverPort();

// Prefix accessors (may include CS2 chat color control bytes).
std::string ChatPrefix();
std::string AdminPrefix();
std::string CaptainPrefixTeam1();
std::string CaptainPrefixTeam2();

bool ConsumeRuChat();
bool ConsumeReadyChat();

// readyup.cfg `dev_bots_ready` (env override: READYUP_DEV_BOTS_READY).
// Logs a loud line whenever the effective value flips to ON.
bool DevBotsReadyEnabled();

// Reloads `readyup.cfg` from disk (best-effort).
// Returns true if the file was successfully read+parsed, false otherwise.
bool ReloadCfg(std::string* err = nullptr);

}  // namespace readyup

