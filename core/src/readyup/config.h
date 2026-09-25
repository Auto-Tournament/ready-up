#pragma once

 #include <string>

namespace readyup {

// Core keys of readyup.cfg (top level, before the first `[section]`). The match flow's keys
// (welcome, ready_hud, hud_*, admin/captain prefixes, consume_ready_chat, dev_bots_*,
// scrim_knife, knife_pick_seconds) are read by plugins/match itself (its config.h).
struct ReadyUpCfg {
  bool debug = false;
  bool banner = true;
  bool chat_debug = false;
  // Chat prefix (token-based; expanded to CS2 chat control bytes at load). Ready Up adds a
  // single space after it.
  std::string chat_prefix;
  // If true, hide `.ru ...` messages from chat after processing.
  bool consume_ru_chat = false;
  // Local status endpoint (docs/FLEET.md §17, status_feed.h). Read once at load.
  bool status_http_enabled = true;
  std::string status_http_bind = "127.0.0.1";
  int status_http_port = 0;  // 0 = game port + 7
  std::string status_http_token;  // empty = generated, kept in csgo/readyup/status.json
  bool status_http_metrics = false;
};

ReadyUpCfg Cfg();

bool DebugEnabled();
bool BannerEnabled();
bool ChatDebugEnabled();

// Chat prefix (may include CS2 chat color control bytes).
std::string ChatPrefix();
bool ConsumeRuChat();

// Reloads `readyup.cfg` from disk (best-effort).
// Returns true if the file was successfully read+parsed, false otherwise.
bool ReloadCfg(std::string* err = nullptr);

}  // namespace readyup

