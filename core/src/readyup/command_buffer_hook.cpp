#include "readyup/command_buffer_hook.h"
#include "readyup/match_end.h"

#include "readyup/admins.h"
#include "readyup/config.h"
#include "readyup/http_client.h"
#include "readyup/logging.h"
#include "readyup/mat_admins.h"
#include "readyup/match_config_parser.h"
#include "readyup/match_token.h"
#include "readyup/minijson.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/persisted_settings.h"
#include "readyup/persisted_match_state.h"
#include "readyup/plugin_loader.h"
#include "readyup/ru_help.h"
#include "readyup/ru_router.h"
#include "readyup/selftest.h"
#include "readyup/sigtest.h"
#include "readyup/webhook.h"
#include "readyup/steamid.h"
#include "readyup/status_feed.h"

#include <dlfcn.h>
#include <link.h>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace readyup {
namespace {

// Mangled name for: CCommandBuffer::AddText(const char*, int, int, bool, double, uint64_t)
static constexpr const char* kAddTextSym = "_ZN14CCommandBuffer7AddTextEPKciibdy";

using AddTextFn = void (*)(void* thisptr, const char* text, int a, int b, bool c, double d, uint64_t e);
AddTextFn g_orig = nullptr;

std::atomic<bool> g_installed{false};

struct LastAddTextCall {
  void* thisptr = nullptr;
  int a = 0;
  int b = 0;
  bool c = false;
  double d = 0.0;
  uint64_t e = 0;
};

std::atomic<void*> g_lastThis{nullptr};
// Remaining params are updated without atomics; benign races are fine (best-effort).
LastAddTextCall g_last{};

static std::string Trim(std::string s) {
  auto is_ws = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && is_ws(static_cast<unsigned char>(s[i]))) ++i;
  if (i) s.erase(0, i);
  return s;
}

static std::vector<std::string> SplitWS(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isspace(c) != 0) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur.push_back(static_cast<char>(c));
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

static bool StartsWithToken(const std::string& s, const char* tok) {
  const size_t n = std::strlen(tok);
  if (s.size() < n) return false;
  if (std::memcmp(s.data(), tok, n) != 0) return false;
  if (s.size() == n) return true;
  const unsigned char next = static_cast<unsigned char>(s[n]);
  return std::isspace(next) != 0 || next == ';' || next == '\n' || next == '\r';
}

static std::string StripCrLf(std::string s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    if (ch == '\r') continue;
    out.push_back(ch);
  }
  return out;
}

static std::string TruncateForPrint(const std::string& s, size_t maxBytes, bool& truncated) {
  truncated = false;
  if (s.size() <= maxBytes) return s;
  truncated = true;
  return s.substr(0, maxBytes);
}

// Match config auth token (set via ru_match_token).
std::atomic<unsigned long long> g_matchReqId{0};

static std::optional<WebhookMatchContext> ParseMatchContextFromJson(const std::string& json, std::string* errOut) {
  return readyup::ParseWebhookMatchContextFromJson(json, errOut);
}

static bool HandleMatchTokenCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_match_token")) return false;

  // `ru_match_token` -> show status only
  // `ru_match_token clear` -> clear
  // `ru_match_token <token...>` -> set
  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    const bool set = readyup::GetMatchTokenCopy().has_value();
    PrintLine(set ? "match token: set" : "match token: not set");
    return true;
  }

  // Everything after the command name is treated as the token (so we don't lose characters).
  const size_t firstWs = t.find_first_of(" \t");
  std::string token = (firstWs == std::string::npos) ? "" : Trim(t.substr(firstWs));

  if (token == "clear") {
    readyup::SetMatchToken("");
    readyup::persisted_settings::PersistMatchToken(std::nullopt);
    PrintLine("match token: cleared");
    return true;
  }

  if (token.empty()) {
    PrintLine("Usage: ru_match_token <token>  (or: ru_match_token clear)");
    return true;
  }

  readyup::SetMatchToken(token);
  readyup::persisted_settings::PersistMatchToken(token);
  PrintLine("match token: set");
  return true;
}

static bool HandleWebhookUrlCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_webhook_url")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    const std::string cur = WebhookBaseUrl();
    Print("webhook url: %s\n", cur.empty() ? "(not set)" : cur.c_str());
    return true;
  }

  const size_t firstWs = t.find_first_of(" \t");
  std::string url = (firstWs == std::string::npos) ? "" : Trim(t.substr(firstWs));
  if (url == "clear") url.clear();

  WebhookConfigure(url);
  if (url.empty()) readyup::persisted_settings::PersistWebhookUrl(std::nullopt);
  else readyup::persisted_settings::PersistWebhookUrl(url);
  WebhookStartSenderThread();
  PrintLine(url.empty() ? "webhook url: cleared" : "webhook url: set");
  return true;
}

static bool HandleHeartbeatUrlCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_heartbeat_url")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    const std::string cur = WebhookHeartbeatUrl();
    Print("heartbeat url: %s\n", cur.empty() ? "(not set)" : cur.c_str());
    return true;
  }

  const size_t firstWs = t.find_first_of(" \t");
  std::string url = (firstWs == std::string::npos) ? "" : Trim(t.substr(firstWs));
  if (url == "clear") url.clear();

  WebhookConfigureHeartbeatUrl(url);
  if (url.empty()) readyup::persisted_settings::PersistHeartbeatUrl(std::nullopt);
  else readyup::persisted_settings::PersistHeartbeatUrl(url);
  WebhookStartSenderThread();
  PrintLine(url.empty() ? "heartbeat url: cleared" : "heartbeat url: set");
  return true;
}

static bool HandleAdminsUrlCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_admins_url")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    const std::string cur = readyup::mat_admins::AdminsUrl();
    Print("admins url: %s\n", cur.empty() ? "(not set)" : cur.c_str());
    return true;
  }

  const size_t firstWs = t.find_first_of(" \t");
  std::string url = (firstWs == std::string::npos) ? "" : Trim(t.substr(firstWs));
  if (url == "clear") url.clear();

  readyup::mat_admins::ConfigureAdminsUrl(std::move(url));
  readyup::mat_admins::RefreshNow();  // fetch admins JSON immediately
  {
    const std::string cur = readyup::mat_admins::AdminsUrl();
    if (cur.empty()) readyup::persisted_settings::PersistAdminsUrl(std::nullopt);
    else readyup::persisted_settings::PersistAdminsUrl(cur);
  }
  PrintLine(readyup::mat_admins::AdminsUrl().empty() ? "admins url: cleared" : "admins url: set");
  return true;
}

static bool HandleAdminsRefreshSecondsCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_admins_refresh_seconds")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("admins refresh seconds: %d\n", readyup::mat_admins::RefreshSeconds());
    return true;
  }

  if (parts.size() >= 2) {
    const int s = std::atoi(parts[1].c_str());
    if (s >= 0) {
      readyup::mat_admins::ConfigureRefreshSeconds(s);
      readyup::persisted_settings::PersistAdminsRefreshSeconds(readyup::mat_admins::RefreshSeconds());
      PrintLine("admins refresh seconds: set");
      return true;
    }
  }

  PrintLine("Usage: ru_admins_refresh_seconds <0|10..3600>");
  return true;
}

static bool HandleWarmupEnableCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_enable")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup enabled: %s (%s)\n", WarmupEnabled() ? "1" : "0",
          WarmupEnabled() ? "matches wait for ready-up" : "matches skip ready-up and knife; live on next round start");
    return true;
  }
  if (parts.size() >= 2) {
    const std::string v = parts[1];
    if (v == "1" || v == "true" || v == "on") {
      SetWarmupEnabled(true);
      PrintLine("warmup enabled: 1 (matches wait for ready-up)");
      return true;
    }
    if (v == "0" || v == "false" || v == "off") {
      SetWarmupEnabled(false);
      PrintLine("warmup enabled: 0 (matches skip ready-up and knife; live on next round start)");
      return true;
    }
  }
  PrintLine("Usage: ru_warmup_enable 0|1");
  return true;
}

// DEBUG ONLY: runtime override of readyup.cfg `dev_bots_scrim` (the live test turns it on
// for one run and back to `cfg` afterwards).
static bool HandleDevBotsScrimCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_dev_bots_scrim")) return false;
  auto parts = SplitWS(t);
  if (parts.size() >= 2) {
    const std::string v = parts[1];
    if (v == "1" || v == "on" || v == "true") SetDevBotsScrimOverride(1);
    else if (v == "0" || v == "off" || v == "false") SetDevBotsScrimOverride(0);
    else if (v == "cfg" || v == "default") SetDevBotsScrimOverride(-1);
    else {
      PrintLine("Usage: ru_dev_bots_scrim 0|1|cfg");
      return true;
    }
  }
  const int ov = DevBotsScrimOverride();
  Print("dev_bots_scrim: %d (%s)\n", DevBotsScrimEnabled() ? 1 : 0,
        ov < 0 ? "from readyup.cfg / READYUP_DEV_BOTS_SCRIM" : "console override; `ru_dev_bots_scrim cfg` clears it");
  return true;
}

static bool HandleCfgExecEnableCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_cfg_exec_enable")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("cfg exec enabled: %s\n", CfgExecEnabled() ? "1" : "0");
    return true;
  }
  if (parts.size() >= 2) {
    const std::string v = parts[1];
    if (v == "1" || v == "true" || v == "on") {
      SetCfgExecEnabled(true);
      PrintLine("cfg exec enabled: 1");
      return true;
    }
    if (v == "0" || v == "false" || v == "off") {
      SetCfgExecEnabled(false);
      PrintLine("cfg exec enabled: 0");
      return true;
    }
  }
  PrintLine("Usage: ru_cfg_exec_enable 0|1");
  return true;
}

static bool HandleWarmupMessageHtmlCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_message_html")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    const std::string cur = WarmupHtmlMessage();
    Print("warmup html: %s\n", cur.empty() ? "(empty)" : cur.c_str());
    return true;
  }

  const size_t firstWs = t.find_first_of(" \t");
  std::string html = (firstWs == std::string::npos) ? "" : Trim(t.substr(firstWs));
  if (html == "clear" || html == "default") html.clear();
  SetWarmupHtmlMessage(std::move(html));
  PrintLine("warmup html: set");
  return true;
}

static bool HandleWarmupRespawnCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_respawn")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup respawn: %s\n", WarmupRespawnEnabled() ? "1" : "0");
    return true;
  }
  const std::string v = parts.size() >= 2 ? parts[1] : "";
  if (v == "1" || v == "true" || v == "on") {
    SetWarmupRespawnEnabled(true);
    PrintLine("warmup respawn: 1");
    return true;
  }
  if (v == "0" || v == "false" || v == "off") {
    SetWarmupRespawnEnabled(false);
    PrintLine("warmup respawn: 0");
    return true;
  }
  PrintLine("Usage: ru_warmup_respawn 0|1");
  return true;
}

static bool HandleWarmupIgnoreWinCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_ignore_win_conditions")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup ignore win conditions: %s\n", WarmupIgnoreWinConditions() ? "1" : "0");
    return true;
  }
  const std::string v = parts.size() >= 2 ? parts[1] : "";
  if (v == "1" || v == "true" || v == "on") {
    SetWarmupIgnoreWinConditions(true);
    PrintLine("warmup ignore win conditions: 1");
    return true;
  }
  if (v == "0" || v == "false" || v == "off") {
    SetWarmupIgnoreWinConditions(false);
    PrintLine("warmup ignore win conditions: 0");
    return true;
  }
  PrintLine("Usage: ru_warmup_ignore_win_conditions 0|1");
  return true;
}

static bool HandleWarmupRoundTimeMinutesCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_roundtime_minutes")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup roundtime minutes: %d\n", WarmupRoundTimeMinutes());
    return true;
  }
  if (parts.size() >= 2) {
    const int m = std::atoi(parts[1].c_str());
    if (m >= 1 && m <= 120) {
      SetWarmupRoundTimeMinutes(m);
      PrintLine("warmup roundtime minutes: set");
      return true;
    }
  }
  PrintLine("Usage: ru_warmup_roundtime_minutes <1..120>");
  return true;
}

static bool HandleWarmupStartMoneyCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_startmoney")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup startmoney: %d\n", WarmupStartMoney());
    return true;
  }
  if (parts.size() >= 2) {
    const int m = std::atoi(parts[1].c_str());
    if (m >= 0 && m <= 60000) {
      SetWarmupStartMoney(m);
      PrintLine("warmup startmoney: set");
      return true;
    }
  }
  PrintLine("Usage: ru_warmup_startmoney <0..60000>");
  return true;
}

static bool HandleWarmupMaxMoneyCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_maxmoney")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup maxmoney: %d\n", WarmupMaxMoney());
    return true;
  }
  if (parts.size() >= 2) {
    const int m = std::atoi(parts[1].c_str());
    if (m >= 0 && m <= 60000) {
      SetWarmupMaxMoney(m);
      PrintLine("warmup maxmoney: set");
      return true;
    }
  }
  PrintLine("Usage: ru_warmup_maxmoney <0..60000>");
  return true;
}

static bool HandleWarmupBuyAnywhereCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_buy_anywhere")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup buy anywhere: %s\n", WarmupBuyAnywhereEnabled() ? "1" : "0");
    return true;
  }
  const std::string v = parts.size() >= 2 ? parts[1] : "";
  if (v == "1" || v == "true" || v == "on") {
    SetWarmupBuyAnywhereEnabled(true);
    PrintLine("warmup buy anywhere: 1");
    return true;
  }
  if (v == "0" || v == "false" || v == "off") {
    SetWarmupBuyAnywhereEnabled(false);
    PrintLine("warmup buy anywhere: 0");
    return true;
  }
  PrintLine("Usage: ru_warmup_buy_anywhere 0|1");
  return true;
}

static bool HandleWarmupInfiniteAmmoCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_infinite_ammo")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup infinite ammo: %s\n", WarmupInfiniteAmmoEnabled() ? "1" : "0");
    return true;
  }
  const std::string v = parts.size() >= 2 ? parts[1] : "";
  if (v == "1" || v == "true" || v == "on") {
    SetWarmupInfiniteAmmoEnabled(true);
    PrintLine("warmup infinite ammo: 1");
    return true;
  }
  if (v == "0" || v == "false" || v == "off") {
    SetWarmupInfiniteAmmoEnabled(false);
    PrintLine("warmup infinite ammo: 0");
    return true;
  }
  PrintLine("Usage: ru_warmup_infinite_ammo 0|1");
  return true;
}

static bool LoadMatchFromUrl(const std::string& url) {
  if (url.empty()) return false;
  const auto token = readyup::GetMatchTokenCopy();
  const bool auth = token.has_value();

  const unsigned long long reqId = g_matchReqId.fetch_add(1, std::memory_order_relaxed) + 1ull;
  Print("match-load[%llu]: GET %s (auth=%s)\n",
        reqId,
        url.c_str(),
        auth ? "on" : "off");

  // NOTE: This is currently synchronous on the server thread. We keep timeouts short in HttpGet().
  const HttpResponse r = HttpGet(url, token);
  if (!r.error.empty()) {
    Print("match-load[%llu]: error: %s (status=%ld)\n", reqId, r.error.c_str(), r.status);
    return true;
  }

  Print("match-load[%llu]: status=%ld bytes=%zu\n", reqId, r.status, r.body.size());

  bool truncated = false;
  const std::string body = TruncateForPrint(r.body, /*maxBytes=*/16384, truncated);
  const std::string safeBody = StripCrLf(body);

  if (truncated) {
    Print("match-load[%llu]: body (first %zu bytes, truncated):\n%s\n", reqId, safeBody.size(), safeBody.c_str());
  } else {
    Print("match-load[%llu]: body:\n%s\n", reqId, safeBody.c_str());
  }

  // Try parse match config and seed webhook context for MAT.
  {
    std::string parseErr;
    auto ctx = ParseMatchContextFromJson(r.body, &parseErr);
    if (!ctx) {
      if (DebugEnabled()) {
        Print("match-load[%llu]: json parse: %s\n", reqId, parseErr.empty() ? "failed" : parseErr.c_str());
      }
    } else {
      WebhookStartSenderThread();
      if (auto prev = WebhookGetMatchContext()) {
        if (prev->matchid != 0 && prev->matchid != ctx->matchid) {
          // Best-effort close out previous series when a new match is loaded.
          WebhookEmitSeriesEnd(/*team1_series_score=*/0, /*team2_series_score=*/0, /*winner=*/"none", /*time_until_restore=*/0);
        }
      }
      WebhookSetMatchContext(*ctx);
      OnMatchLoaded();
      WebhookEmitSeriesStart();

      // Persist match config so a rebooted server can restore without MAT re-init.
      readyup::persisted_match_state::PersistActiveMatchJson(r.body);

      // Human matches generally should not start with CS2 auto-spawned bots.
      // Some server configs/gamemode cfgs will create fill bots when empty.
      // Kick any existing bots and set bot_quota=0 as a best-effort mitigation.
      // (These commands are safe no-ops if bots are disabled.)
      (void)EnqueueServerCommand("bot_kick");
      (void)EnqueueServerCommand("bot_quota 0");

      // Enable CS2 round backups for recovery.
      // Prefix includes matchid and map number to avoid collisions.
      {
        const int mapNumber = 1;
        const std::string prefix =
            "readyup_backup_" + std::to_string(static_cast<unsigned long long>(ctx->matchid)) +
            "_map" + std::to_string(mapNumber) + "_";
        (void)EnqueueServerCommand("mp_backup_round_auto 1");
        (void)EnqueueServerCommand("mp_backup_restore_load_autopause 1");
        const std::string cmd = "mp_backup_round_file " + prefix;
        (void)EnqueueServerCommand(cmd.c_str());
        readyup::persisted_match_state::PersistBackupPrefix(prefix);
      }

      // Force-load map 1 when maplist is provided.
      auto isSafeMapName = [](const std::string& s) -> bool {
        if (s.empty()) return false;
        for (unsigned char c : s) {
          // Allow workshop-like paths and common map chars, but keep injection-safe (no spaces/quotes/;).
          if (c == ';' || c == '\n' || c == '\r' || c == '"' || c == '\\' || std::isspace(c) != 0) return false;
          if (!(std::isalnum(c) != 0 || c == '_' || c == '/' || c == '.' || c == '-')) return false;
        }
        return true;
      };

      if (!ctx->maplist.empty()) {
        const std::string& map1 = ctx->maplist[0];
        const auto ms = MatchStateGet();
        if (isSafeMapName(map1) && ms.current_map != map1) {
          const std::string cmd = "changelevel " + map1;
          (void)EnqueueServerCommand(cmd.c_str());
        }
      }

      // Print a concise parsed summary (the raw JSON is already printed above).
      Print("match-load[%llu]: parsed: teams=\"%s\" vs \"%s\" roster=%zu map1=%s maxRounds=%d ot=%s seg=%d maxOT=%d dmgTiebreak=%s suddenDeath=%s\n",
            reqId,
            ctx->team1_name.c_str(),
            ctx->team2_name.c_str(),
            ctx->roster_team.size(),
            ctx->maplist.empty() ? "(none)" : ctx->maplist[0].c_str(),
            ctx->maxRounds,
            ctx->overtime_enabled ? "on" : "off",
            ctx->overtimeSegments,
            ctx->maxOvertimes,
            ctx->damageTiebreakEnabled ? "on" : "off",
            ctx->suddenDeathOnDamageTie ? "on" : "off");

      Print("match-load[%llu]: match context set: matchid=%llu slug=%s\n",
            reqId,
            static_cast<unsigned long long>(ctx->matchid),
            ctx->slug.empty() ? "(none)" : ctx->slug.c_str());
    }
  }
  return true;
}

static bool HandleRuCommandLine(const std::string& line) {
  // Accept `ru ...` from server console (not `.ru`).
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru")) return false;

  auto parts = SplitWS(t);
  if (parts.empty()) return false;

  // `ru` / `ru help`
  if (parts.size() == 1 || (parts.size() >= 2 && parts[1] == "help")) {
    PrintRuHelp();
    return true;
  }

  if (parts[1] == "admins") {
    std::vector<std::string> args;
    if (parts.size() > 2) args.assign(parts.begin() + 2, parts.end());
    HandleAdminsCommand(/*senderSteamid64=*/0, /*senderName=*/"Console", args);
    return true;
  }

  if (parts[1] == "plugin" || parts[1] == "plugins") {
    const std::vector<std::string> args(parts.begin() + 2, parts.end());
    plugins::HandlePluginCommand(args, /*replyToChat=*/false);
    return true;
  }

  if (parts[1] == "sigtest") {
    const bool ok = RunSigTest(/*verbose=*/true);
    PrintLine(ok ? "sigtest OK" : "sigtest FAIL");
    return true;
  }

  if (parts[1] == "selftest") {
    (void)RunSelftest(/*printToConsole=*/true);
    return true;
  }

  if (parts[1] == "mode") {
    if (parts.size() == 2) {
      Print("mode: %s\n", GetModeString());
      return true;
    }
    if (parts.size() >= 3 && parts[2] == "idle") {
      // Unify console and chat semantics: `.ru idle` clears match context + persisted match.
      RouteChatCommand(/*steamid64=*/0, /*playerName=*/"Console", /*text=*/".ru idle");
      return true;
    }
    if (parts.size() >= 3 && parts[2] == "practice") {
      RouteChatCommand(/*steamid64=*/0, /*playerName=*/"Console", /*text=*/".ru practice");
      return true;
    }
    PrintLine("Usage: ru mode [idle|practice]");
    return true;
  }

  // Convenience aliases (space-based).
  if (parts[1] == "idle") {
    RouteChatCommand(/*steamid64=*/0, /*playerName=*/"Console", /*text=*/".ru idle");
    return true;
  }
  if (parts[1] == "practice") {
    RouteChatCommand(/*steamid64=*/0, /*playerName=*/"Console", /*text=*/".ru practice");
    return true;
  }
  if (parts[1] == "scrim") {
    RouteChatCommand(/*steamid64=*/0, /*playerName=*/"Console", /*text=*/".ru scrim");
    return true;
  }
  if (parts[1] == "status_http") {
    for (const auto& l : status_feed::StatusLines()) Print("%s\n", l.c_str());
    return true;
  }

  if (parts[1] == "state" || parts[1] == "status") {
    RouteChatCommand(/*steamid64=*/0, /*playerName=*/"Console", /*text=*/".ru state");
    return true;
  }

  if (parts[1] == "reload") {
    std::string err;
    if (!ReloadCfg(&err)) {
      PrintLine(err.empty() ? "reload: failed" : ("reload: failed: " + err).c_str());
      return true;
    }
    PrintLine("reload: ok");
    return true;
  }

  if (parts[1] == "match") {
    if (parts.size() >= 3 && parts[2] == "load") {
      if (parts.size() < 4) {
        PrintLine("Usage: ru match load <url>");
        return true;
      }
      (void)LoadMatchFromUrl(parts[3]);
      return true;
    }
    PrintLine("Usage: ru match load <url>");
    return true;
  }

  if (parts[1] == "side") {
    auto ctx = WebhookGetMatchContext();
    if (!ctx) {
      PrintLine("side: no match loaded");
      return true;
    }
    if (!KnifeIsAwaitingPick()) {
      PrintLine("side: no knife side pick pending");
      return true;
    }
    if (parts.size() < 3) {
      PrintLine("Usage: ru side <stay|switch|ct|t>");
      return true;
    }
    const std::string choice = parts[2];
    if (!KnifeApplySideChoice(choice, /*pickerSteamid64=*/0, /*pickerName=*/"Console", /*isAdminOverride=*/true)) {
      PrintLine("side: invalid choice (use: stay|switch|ct|t)");
      return true;
    }
    PrintLine("side: ok");
    return true;
  }

  if (parts[1] == "start") {
    if (!WebhookGetMatchContext()) {
      PrintLine("start: no match loaded");
      return true;
    }
    (void)ForceStartMatch();
    PrintLine("start: ok");
    return true;
  }

  if (parts[1] == "restart") {
    if (!WebhookGetMatchContext()) {
      PrintLine("restart: no match loaded");
      return true;
    }
    (void)RestartMatch();
    PrintLine("restart: ok");
    return true;
  }

  if (parts[1] == "end") {
    if (!WebhookGetMatchContext()) {
      PrintLine("end: no match loaded");
      return true;
    }
    WebhookEmitSeriesEnd(/*team1_series_score=*/0, /*team2_series_score=*/0, /*winner=*/"none", /*time_until_restore=*/0);
    WebhookClearMatchContext();
    (void)EndMatchResetServer();
    PrintLine("end: ok");
    return true;
  }

  if (parts[1] == "recover") {
    if (!WebhookGetMatchContext()) {
      PrintLine("recover: no match loaded");
      return true;
    }
    int round = 0;
    if (parts.size() >= 3) {
      try {
        round = std::stoi(parts[2]);
      } catch (...) {
        round = 0;
      }
    }
    round = std::max(0, round);
    const auto ms = MatchStateGet();
    WebhookEmitRecoverRequested(ms.map_number, round);
    PrintLine("recover: ok");
    return true;
  }

  // `ru <sub>` a plugin registered (register_ru_subcommand); runs on the next GameFrame.
  if (plugins::TryDispatchRu(/*console=*/true, 0, "Console", t)) return true;

  // `ru <cmd> ...` reaches a console command a plugin registered, so plugins can offer
  // `ru fleet status` next to the core's own `ru ...` commands (runs on the next GameFrame).
  {
    std::string rest;
    for (size_t i = 1; i < parts.size(); ++i) rest += (i > 1 ? " " : "") + parts[i];
    if (plugins::TryDispatchConsole(rest)) return true;
  }

  PrintLine("Unknown Ready Up command. Try: ru help");
  return true;
}

static bool HandleReadyUpConsoleCommandLine(const std::string& line) {
  // RU_CMD_OBSERVE console registrations see the line; it still runs normally.
  plugins::ObserveConsole(Trim(line));
  // Handle standalone Ready Up console/RCON commands (not part of `ru ...`).
  if (HandleMatchTokenCommand(line)) return true;
  if (HandleWebhookUrlCommand(line)) return true;
  if (HandleHeartbeatUrlCommand(line)) return true;
  if (HandleAdminsUrlCommand(line)) return true;
  if (HandleAdminsRefreshSecondsCommand(line)) return true;
  if (HandleCfgExecEnableCommand(line)) return true;
  if (HandleWarmupEnableCommand(line)) return true;
  if (HandleDevBotsScrimCommand(line)) return true;
  if (HandleWarmupMessageHtmlCommand(line)) return true;
  if (HandleWarmupRespawnCommand(line)) return true;
  if (HandleWarmupIgnoreWinCommand(line)) return true;
  if (HandleWarmupRoundTimeMinutesCommand(line)) return true;
  if (HandleWarmupStartMoneyCommand(line)) return true;
  if (HandleWarmupMaxMoneyCommand(line)) return true;
  if (HandleWarmupBuyAnywhereCommand(line)) return true;
  if (HandleWarmupInfiniteAmmoCommand(line)) return true;
  // Demo recording/upload, series-end kick delays, ru_match_stats (match_end.h).
  if (MatchFlowHandleConsoleLine(Trim(line))) return true;

  // Handle `ru ...` command family.
  if (HandleRuCommandLine(line)) return true;

  // Console commands registered by plugins (core commands above always win). The
  // plugin callback runs on the next GameFrame, never inside AddText.
  return plugins::TryDispatchConsole(Trim(line));
}

static void Hook_AddText(void* thisptr, const char* text, int a, int b, bool c, double d, uint64_t e) {
  // Capture last known command buffer invocation so we can enqueue commands later.
  g_last.thisptr = thisptr;
  g_last.a = a;
  g_last.b = b;
  g_last.c = c;
  g_last.d = d;
  g_last.e = e;
  g_lastThis.store(thisptr, std::memory_order_release);

  // Intercept only simple single-line `ru ...` commands.
  // We intentionally do NOT try to parse complex command buffers.
  if (text) {
    const std::string s(text);
    // Use first line only.
    const size_t nl = s.find_first_of("\r\n");
    const std::string first = (nl == std::string::npos) ? s : s.substr(0, nl);
    if (HandleReadyUpConsoleCommandLine(first)) {
      // Swallow this command so the engine doesn't try to interpret it as an alias/unknown.
      return;
    }
  }

  if (g_orig) g_orig(thisptr, text, a, b, c, d, e);
}

// Minimal ELF PLT/GOT patcher for the current process.
struct ElfDynInfo {
  const ElfW(Sym)* symtab = nullptr;
  const char* strtab = nullptr;
  size_t syment = 0;
  const ElfW(Rela)* jmprel_rela = nullptr;
  size_t pltrelsz = 0;
  const ElfW(Rel)* jmprel_rel = nullptr;
  bool rela = true;
};

static bool ReadDynInfo(struct dl_phdr_info* info, ElfDynInfo& out) {
  const ElfW(Phdr)* phdr = info->dlpi_phdr;
  for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
    if (phdr[i].p_type != PT_DYNAMIC) continue;
    const ElfW(Dyn)* dyn = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + phdr[i].p_vaddr);

    ElfW(Addr) symtab = 0;
    ElfW(Addr) strtab = 0;
    ElfW(Addr) jmprel = 0;
    size_t pltrelsz = 0;
    size_t syment = 0;
    ElfW(Sword) pltrel = DT_RELA;

    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
      switch (d->d_tag) {
        case DT_SYMTAB: symtab = d->d_un.d_ptr; break;
        case DT_STRTAB: strtab = d->d_un.d_ptr; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltrelsz = static_cast<size_t>(d->d_un.d_val); break;
        case DT_SYMENT: syment = static_cast<size_t>(d->d_un.d_val); break;
        case DT_PLTREL: pltrel = d->d_un.d_val; break;
        default: break;
      }
    }

    if (!symtab || !strtab || !jmprel || !pltrelsz) return false;

    out.symtab = reinterpret_cast<const ElfW(Sym)*>(symtab);
    out.strtab = reinterpret_cast<const char*>(strtab);
    out.syment = syment;
    out.pltrelsz = pltrelsz;
    out.rela = (pltrel == DT_RELA);
    if (out.rela) out.jmprel_rela = reinterpret_cast<const ElfW(Rela)*>(jmprel);
    else out.jmprel_rel = reinterpret_cast<const ElfW(Rel)*>(jmprel);
    return true;
  }
  return false;
}

static bool PatchGotEntry(void** got, void* replacement, void** outOld) {
  if (!got || !replacement) return false;
  const uintptr_t addr = reinterpret_cast<uintptr_t>(got);
  const uintptr_t page = addr & ~(static_cast<uintptr_t>(getpagesize() - 1));
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(getpagesize()), PROT_READ | PROT_WRITE) != 0) {
    return false;
  }
  if (outOld) *outOld = *got;
  *got = replacement;
  // Restore to RX/R (best-effort).
  mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(getpagesize()), PROT_READ);
  return true;
}

static int PatchModule(struct dl_phdr_info* info, size_t, void* /*data*/) {
  // Skip the main executable name empty? still patch.
  ElfDynInfo dyn{};
  if (!ReadDynInfo(info, dyn)) return 0;

  auto patchOne = [&](ElfW(Addr) r_offset, unsigned long symIndex) {
    const ElfW(Sym)& sym = dyn.symtab[symIndex];
    const char* name = dyn.strtab + sym.st_name;
    if (!name) return;
    if (std::strcmp(name, kAddTextSym) != 0) return;

    void** got = reinterpret_cast<void**>(info->dlpi_addr + r_offset);
    void* old = nullptr;
    if (PatchGotEntry(got, reinterpret_cast<void*>(Hook_AddText), &old)) {
      if (!g_orig) g_orig = reinterpret_cast<AddTextFn>(old);
      if (DebugEnabled()) {
        Print("console: hooked CCommandBuffer::AddText in module: %s\n", info->dlpi_name && *info->dlpi_name ? info->dlpi_name : "(main)");
      }
    }
  };

  if (dyn.rela && dyn.jmprel_rela) {
    const size_t n = dyn.pltrelsz / sizeof(ElfW(Rela));
    for (size_t i = 0; i < n; ++i) {
      const ElfW(Rela)& r = dyn.jmprel_rela[i];
      const unsigned long symIndex = ELF64_R_SYM(r.r_info);
      patchOne(r.r_offset, symIndex);
    }
  } else if (!dyn.rela && dyn.jmprel_rel) {
    const size_t n = dyn.pltrelsz / sizeof(ElfW(Rel));
    for (size_t i = 0; i < n; ++i) {
      const ElfW(Rel)& r = dyn.jmprel_rel[i];
      const unsigned long symIndex = ELF64_R_SYM(r.r_info);
      patchOne(r.r_offset, symIndex);
    }
  }

  return 0;
}

}  // namespace

void InstallCommandBufferHook() {
  bool expected = false;
  if (!g_installed.compare_exchange_strong(expected, true)) return;

  dl_iterate_phdr(PatchModule, nullptr);

  if (!g_orig && DebugEnabled()) {
    PrintLine("console: failed to hook CCommandBuffer::AddText (console `ru` commands disabled).");
  }
}

bool EnqueueServerCommand(const char* text) {
  if (!text || !*text) return false;
  void* thisptr = g_lastThis.load(std::memory_order_acquire);
  if (!thisptr || !g_orig) return false;

  // Ensure newline termination (most engine command buffer users expect it).
  std::string s(text);
  if (s.back() != '\n') s.push_back('\n');

  // Reuse the latest parameter values the engine called AddText with.
  // This keeps us ABI-stable without guessing flag meanings.
  g_orig(thisptr, s.c_str(), g_last.a, g_last.b, g_last.c, g_last.d, g_last.e);
  return true;
}

}  // namespace readyup


namespace readyup {

bool CommandBufferHookInstalled() { return g_orig != nullptr; }

bool CommandBufferSeen() { return g_lastThis.load(std::memory_order_acquire) != nullptr; }

}  // namespace readyup
