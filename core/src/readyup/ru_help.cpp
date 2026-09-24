#include "readyup/ru_help.h"

#include "readyup/logging.h"
#include "readyup/version.h"

namespace readyup {

void PrintRuHelp() {
  static constexpr const char* kBanner = R"(
 ______     ______     ______     _____     __  __        __  __     ______  
╱╲  == ╲   ╱╲  ___╲   ╱╲  __ ╲   ╱╲  __─.  ╱╲ ╲_╲ ╲      ╱╲ ╲╱╲ ╲   ╱╲  == ╲ 
╲ ╲  __<   ╲ ╲  __╲   ╲ ╲  __ ╲  ╲ ╲ ╲╱╲ ╲ ╲ ╲____ ╲     ╲ ╲ ╲_╲ ╲  ╲ ╲  _─╱ 
 ╲ ╲_╲ ╲_╲  ╲ ╲_____╲  ╲ ╲_╲ ╲_╲  ╲ ╲____─  ╲╱╲_____╲     ╲ ╲_____╲  ╲ ╲_╲   
  ╲╱_╱ ╱_╱   ╲╱_____╱   ╲╱_╱╲╱_╱   ╲╱____╱   ╲╱_____╱      ╲╱_____╱   ╲╱_╱   
)";

  // PrintRaw formats into a 2048-byte buffer; keep each call well below that.
  PrintRaw("%s\n", kBanner);
  PrintRaw(
      "\n"
      "[ReadyUp] build: %s\n"
      "Author: Sivert Gullberg Hansen\n"
      "Repo:   github.com/Auto-Tournament/ready-up\n"
      "\n"
      "Server console / RCON commands:\n"
      "  - ru                     (prints this help)\n"
      "  - ru_match_token <token>  (sets Bearer token)\n"
      "  - ru_match_token clear\n"
      "  - ru_webhook_url <base>   (e.g. https://mat/api/events)\n"
      "  - ru_webhook_url clear\n"
      "  - ru_heartbeat_url <url>  (full URL incl /api/servers/:id/heartbeat)\n"
      "  - ru_heartbeat_url clear\n"
      "  - ru_admins_url <url>|clear\n"
      "  - ru_admins_refresh_seconds <10..3600>\n"
      "  - ru_cfg_exec_enable 0|1  (exec ReadyUp/*.cfg on mode transitions)\n"
      "  - ru match load <url>\n"
      "  - ru mode                 (prints current mode)\n"
      "  - ru mode idle\n"
      "  - ru mode practice\n"
      "  - ru idle                 (alias for: ru mode idle)\n"
      "  - ru practice             (alias for: ru mode practice)\n"
      "  - ru scrim                (re-enable auto scrim warmup after ru idle)\n"
      "  - ru state                (mode, roster, ready, bots, flags)\n"
      "  - ru reload               (reload readyup_cfg.json)\n"
      "  - ru selftest             (engine surface, hooks, schema, events, DB, features; PASS/FAIL)\n"
      "  - ru start\n"
      "  - ru restart\n"
      "  - ru end\n"
      "  - ru recover [round_number]\n"
      "  - ru side <stay|switch|ct|t>\n"
      "  - ru plugin list\n"
      "  - ru plugin load|unload|reload <name>   (csgo/readyup/plugins/<name>.so)\n"
      "  - ru_warmup_enable 0|1\n"
      "  - ru_warmup_message_html <html...>\n"
      "  - ru_warmup_message_html default\n"
      "  - ru_warmup_respawn 0|1\n"
      "  - ru_warmup_ignore_win_conditions 0|1\n"
      "  - ru_warmup_roundtime_minutes <1..120>\n"
      "  - ru_warmup_startmoney <0..60000>\n"
      "  - ru_warmup_maxmoney <0..60000>\n"
      "  - ru_warmup_buy_anywhere 0|1\n"
      "  - ru_warmup_infinite_ammo 0|1   (1 => sv_infinite_ammo 2)\n"
      "  - ru admins\n"
      "  - ru admins add <steamid64|name>\n"
      "  - ru admins remove <steamid64|name>\n",
      BuildVersion());
  PrintRaw(
      "\n"
      "Chat commands:\n"
      "  - .r              (toggle ready)\n"
      "  - .ready\n"
      "  - .unready / .ur / .notready / .nr\n"
      "  - .pause / .p / .tech\n"
      "  - .unpause / .up  (requires both teams)\n"
      "  - .stay / .switch / .swap / .ct / .t  (knife-winning captain or admin)\n"
      "  - .gg\n"
      "  - .ff / .forfeit  (captain-only; requires captains in match config)\n"
      "  - .ru state       (anyone: mode, roster, ready, flags)\n"
      "\n"
      "Admin chat commands:\n"
      "  - .ru admins\n"
      "  - .ru admins add <steamid64|name>\n"
      "  - .ru admins remove <steamid64|name>\n"
      "  - .ru reload\n"
      "  - .ru selftest    (full report in the server console, summary in chat)\n"
      "  - .ru prac\n"
      "  - .ru idle        (sticky: no auto scrim warmup until .ru scrim / map change)\n"
      "  - .ru scrim\n"
      "  - .ru start\n"
      "  - .ru restart\n"
      "  - .ru end\n"
      "  - .ru recover [round_number]\n"
      "  - .ru side <stay|switch|ct|t>  (knife-winning captain or admin)\n"
      "  - .ru pause / .ru fp / .ru forcepause\n"
      "  - .ru unpause / .ru fup / .ru forceunpause\n"
      "\n"
      "Notes:\n"
      "  - add/remove are admin-only.\n"
      "  - first admin must be added from server console or seeded in DB.\n"
      "  - admins/captains may be chat-prefixed via readyup.cfg (admin_prefix / captain_prefix_*).\n"
      "    - if IVEngineServer::ClientCommand is available, prefixing uses a temporary name swap.\n"
      "    - otherwise Ready Up relays a prefixed chat line and consumes the original to avoid duplicates.\n"
      "  - `.ru` and `.r` visibility can be toggled via readyup.cfg (consume_ru_chat / consume_ready_chat).\n"
      "  - when a match is loaded, Ready Up enables whitelist + team enforcement:\n"
      "    - non-roster/non-spectator players are kicked (admins are exempt)\n"
      "    - jointeam is forced based on roster + map_sides (when provided)\n"
      "  - no match loaded: idle -> scrim warmup when a human joins CT/T; everyone on CT/T\n"
      "    types .r -> 5s countdown -> live (scrims skip whitelist/team enforcement).\n");
}

}  // namespace readyup

