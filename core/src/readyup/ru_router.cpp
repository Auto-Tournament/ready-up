#include "readyup/ru_router.h"

#include "readyup/admins.h"
#include "readyup/chat.h"
#include "readyup/config.h"
#include "readyup/command_buffer_hook.h"
#include "readyup/features.h"
#include "readyup/selftest.h"
#include "readyup/admin_check.h"
#include "readyup/logging.h"
#include "readyup/game_events.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/pause_state.h"
#include "readyup/postgres.h"
#include "readyup/scrim_flow.h"
#include "readyup/persisted_match_state.h"
#include "readyup/plugin_loader.h"
#include "readyup/ready_hud.h"
#include "readyup/slot_registry.h"
#include "readyup/version.h"
#include "readyup/webhook.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

static std::string Trim(std::string s) {
  auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
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

static bool ShouldProcess(uint64_t steamid64, const std::string& playerName, const std::string& text) {
  // Multiple interception paths can observe the same chat message (e.g. Host_Say detour
  // + in-process log listener). Keep a tiny short-lived cache to suppress duplicates.
  using Clock = std::chrono::steady_clock;
  static std::mutex mu;
  static std::deque<std::pair<Clock::time_point, std::string>> recent;

  const auto now = Clock::now();
  const auto ttl = std::chrono::milliseconds(400);

  {
    std::lock_guard<std::mutex> lock(mu);
    while (!recent.empty() && (now - recent.front().first) > ttl) recent.pop_front();
  }

  std::string key;
  key.reserve(text.size() + 48);
  if (steamid64 != 0) {
    key = std::to_string(steamid64);
  } else {
    key = playerName;
  }
  key.push_back('|');
  key += text;

  {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& it : recent) {
      if (it.second == key) return false;
    }
    recent.emplace_back(now, std::move(key));
    while (recent.size() > 32) recent.pop_front();
  }
  return true;
}

static bool IsPlayerChatCommand(const std::string& s) {
  return s == ".r" || s == ".ready" || s == ".unready" || s == ".ur" || s == ".notready" || s == ".nr" ||
         s == ".pause" || s == ".p" || s == ".tech" || s == ".unpause" || s == ".up" || s == ".gg" ||
         s == ".ff" || s == ".forfeit" ||
         // MatchZy-style knife side pick shortcuts (forwarded to: `.ru side ...`)
         s == ".stay" || s == ".switch" || s == ".swap" || s == ".ct" || s == ".t" ||
         s == ".help" ||
         // MatchZy-style practice aliases (admin-only).
         s == ".prac" || s == ".tactics" ||
         // Practice utilities (only active in practice mode).
         s == ".bot" || s == ".cbot" || s == ".crouchbot" || s == ".boost" || s == ".crouchboost" || s == ".nobots";
}

}  // namespace

namespace readyup {

bool IsCoreChatCommand(const std::string& firstToken) {
  return firstToken == ".ru" || IsPlayerChatCommand(firstToken);
}

void RouteChatCommand(uint64_t steamid64, const std::string& playerName, const std::string& text) {
  const std::string t = Trim(text);
  Debug("ru: RouteChatCommand steamid64=%llu name=\"%s\" text=\"%s\"\n",
        static_cast<unsigned long long>(steamid64),
        playerName.c_str(),
        t.c_str());
  if (t.empty()) {
    DebugLine("ru: ignored (empty after trim)");
    return;
  }
  // Player chat needs a chat source and a way to answer; the console path always works.
  if (steamid64 != 0 && !FeatureEnabled(Feature::ChatCommands)) return;
  if (!ShouldProcess(steamid64, playerName, t)) {
    DebugLine("ru: ignored (dedupe)");
    return;
  }

  auto parts = SplitWS(t);
  if (parts.empty()) {
    DebugLine("ru: ignored (no parts)");
    return;
  }
  const std::string& first = parts[0];

  auto isPlayerCmd = [](const std::string& s) { return IsPlayerChatCommand(s); };

  // Commands owned by a loaded plugin (never a core command; see IsCoreChatCommand).
  // The plugin callback runs on the next GameFrame.
  if (!IsCoreChatCommand(first) && plugins::TryDispatchChat(steamid64, playerName, t)) {
    Debug("ru: \"%s\" queued for its plugin\n", first.c_str());
    return;
  }

  const bool isRu = (first == ".ru");
  const bool isPlayer = isPlayerCmd(first);
  if (!isRu && !isPlayer) {
    Debug("ru: ignored (not .ru or player cmd) first=\"%s\"\n", first.c_str());
    return;
  }

  if (isPlayer) {
    // Player-only commands.
    if (steamid64 == 0) return;
    auto ctx = WebhookGetMatchContext();
    const bool hasMatch = static_cast<bool>(ctx);

    if (first == ".help") {
      // Keep it short; send a couple of lines.
      SendToChat("Ready Up commands: .r / .ready / .ur (.nr) | .pause (.tech) | .unpause");
      if (hasMatch) {
        SendToChat("Ready Up: knife: .stay/.switch (.ct/.t) | forfeit: .ff (captain)");
      } else {
        SendToChat(Cfg().scrim_knife
                       ? "Ready Up: scrim: when everyone on CT/T is READY: 5s countdown, knife round, winners .stay/.switch, live."
                       : "Ready Up: scrim: when everyone on CT/T is READY, a 5s countdown starts and the scrim goes live.");
      }
      return;
    }

    if (first == ".prac" || first == ".tactics") {
      // MatchZy-style alias: forward to admin command handler.
      RouteChatCommand(steamid64, playerName, ".ru prac");
      return;
    }

    // MatchZy-style practice bot helpers (minimal server-command parity).
    if (first == ".nobots") {
      if (GetMode() != ReadyUpMode::Practice) {
        SendToChat("Ready Up: .nobots is only available in practice mode.");
        return;
      }
      (void)EnqueueServerCommand("bot_kick");
      SendToChat("Ready Up: bots removed.");
      return;
    }

    if (first == ".bot" || first == ".cbot" || first == ".crouchbot" || first == ".boost" || first == ".crouchboost") {
      if (GetMode() != ReadyUpMode::Practice) {
        SendToChat("Ready Up: bot commands are only available in practice mode.");
        return;
      }

      // Best-effort: spawn a single bot (MatchZy spawns at player pos; we use server commands).
      // Prefer opposite team to the player (CT->T bot, T->CT bot).
      int tn = 0;
      // Team from the SteamID-keyed human table (log lines + player_team events).
      for (const auto& h : ListHumans()) {
        if (h.steamid64 == steamid64) {
          tn = h.team;
          break;
        }
      }

      const bool crouch = (first == ".cbot" || first == ".crouchbot" || first == ".crouchboost");
      if (crouch) {
        (void)EnqueueServerCommand("bot_crouch 1");
      }

      if (tn == 3) {  // player CT -> spawn T bot
        (void)EnqueueServerCommand("bot_join_team T");
        (void)EnqueueServerCommand("bot_add_t");
      } else {        // default spawn CT bot
        (void)EnqueueServerCommand("bot_join_team CT");
        (void)EnqueueServerCommand("bot_add_ct");
      }

      (void)EnqueueServerCommand("bot_stop 1");
      (void)EnqueueServerCommand("bot_freeze 1");
      (void)EnqueueServerCommand("bot_zombie 1");
      if (crouch) {
        // Reset global bot crouch toggle so future bots aren't forced unless requested.
        (void)EnqueueServerCommand("bot_crouch 0");
      }
      SendToChat(crouch ? "Ready Up: crouch bot added." : "Ready Up: bot added.");
      return;
    }

    // Below this point: match / scrim ready-state commands require team membership.
    ScrimRoster scrim;
    if (!hasMatch) scrim = BuildScrimRoster();

    if (hasMatch) {
      if (ctx->roster_team.find(steamid64) == ctx->roster_team.end()) {
        DebugLine("ru: match loaded but sender is not on its roster");
        SendToChat("Ready Up: you are not on this match's roster.");
        return;
      }
    } else {
      // Pre-match scrim: only ready/unready work; say so instead of staying silent.
      if (first != ".r" && first != ".ready" && first != ".unready" && first != ".ur" && first != ".notready" && first != ".nr") {
        if (first == ".stay" || first == ".switch" || first == ".swap" || first == ".ct" || first == ".t") {
          SendToChat("Ready Up: no knife side pick pending (no match loaded).");
        } else {
          // .pause/.p/.tech/.unpause/.up/.gg/.ff/.forfeit
          SendToChat((std::string("Ready Up: ") + first + " only works during a live match.").c_str());
        }
        return;
      }
      const ReadyUpMode curMode = GetMode();
      if (curMode == ReadyUpMode::Practice) {
        SendToChat("Ready Up: ready-up is not used in practice mode.");
        return;
      }
      if (curMode == ReadyUpMode::Idle && !ScrimAutoEnabled()) {
        SendToChat("Ready Up: scrim warmup is off (admin: .ru scrim to enable).");
        return;
      }
      if (scrim.teamNum.find(steamid64) == scrim.teamNum.end()) {
        Debug("ru: scrim roster has %zu players, %zu spectators; sender not on CT/T\n",
              scrim.teamNum.size(), scrim.spectators.size());
        for (const auto& h : readyup::ListHumans()) {
          Debug("ru:   human userid=%d steamid64=%llu name=\"%s\" team=%d\n", h.userid,
                static_cast<unsigned long long>(h.steamid64), h.name.c_str(), h.team);
        }
        for (const auto& b : readyup::ListBots()) {
          Debug("ru:   bot userid=%d name=\"%s\" team=%d (dev_bots_ready=%d)\n", b.userid, b.name.c_str(), b.team,
                DevBotsReadyEnabled() ? 1 : 0);
        }
        SendToChat("Ready Up: join CT or T first.");
        return;
      }
    }

    auto computeCounts = [&]() {
      int r1 = 0, r2 = 0, total = 0;
      int expected = 0;
      if (hasMatch) {
        expected = static_cast<int>(ctx->roster_team.size());
        for (const auto& kv : ctx->roster_team) {
          const uint64_t sid = kv.first;
          if (sid == 0) continue;
          if (!IsReady(sid)) continue;
          total++;
          if (kv.second == WebhookTeam::Team1) r1++;
          else if (kv.second == WebhookTeam::Team2) r2++;
        }
      } else {
        expected = static_cast<int>(scrim.teamNum.size());
        for (const auto& kv : scrim.teamNum) {
          const uint64_t sid = kv.first;
          const int tnum = kv.second;
          if (!IsReady(sid)) continue;
          total++;
          if (tnum == 3) r1++;      // CT => team1
          else if (tnum == 2) r2++; // T  => team2
        }
      }
      struct Out {
        int r1, r2, total, expected;
      };
      return Out{r1, r2, total, expected};
    };

    auto emitReady = [&](bool nowReady) {
      const auto c = computeCounts();
      WebhookTeam team = WebhookTeam::Unknown;
      if (hasMatch) team = ctx->roster_team[steamid64];
      else {
        const int tnum = scrim.teamNum.at(steamid64);
        team = (tnum == 3) ? WebhookTeam::Team1 : (tnum == 2) ? WebhookTeam::Team2 : WebhookTeam::Unknown;
      }
      WebhookEmitPlayerReady(WebhookPlayer{steamid64, playerName, team}, nowReady, c.r1, c.r2, c.total, c.expected);
    };

    // Not a toggle: `.r`/`.ready` always mean READY, `.ur`/`.nr`/`.unready` always NOT READY.
    auto progress = [&]() {
      const auto c = computeCounts();
      return "(" + std::to_string(c.total) + "/" + std::to_string(c.expected) + " ready)";
    };
    if (first == ".r" || first == ".ready") {
      const bool wasReady = SetReady(steamid64, true);
      if (wasReady) {
        if (!HudReplacesChat()) SendToChat(("Ready Up: " + playerName + " is already READY " + progress() + ".").c_str());
      } else {
        if (!HudReplacesChat()) SendToChat(("Ready Up: " + playerName + " is now READY " + progress() + ".").c_str());
        emitReady(true);
      }
      // Scrim: the countdown / go-live is driven by ScrimTick (GameFrame).
      ScrimNoteReadyChanged();
      return;
    }
    if (first == ".unready" || first == ".ur" || first == ".notready" || first == ".nr") {
      const bool wasReady = SetReady(steamid64, false);
      if (!wasReady) {
        if (!HudReplacesChat()) SendToChat(("Ready Up: " + playerName + " is already NOT READY " + progress() + ".").c_str());
      } else {
        if (!HudReplacesChat()) SendToChat(("Ready Up: " + playerName + " is now NOT READY " + progress() + ".").c_str());
        emitReady(false);
      }
      ScrimNoteReadyChanged();
      return;
    }
    const bool isPauseCmd =
        first == ".pause" || first == ".p" || first == ".tech" || first == ".unpause" || first == ".up";
    if (isPauseCmd && !FeatureEnabled(Feature::Pauses)) {
      SendToChat("Ready Up: pauses are unavailable on this server build (see `ru selftest`).");
      return;
    }
    if (first == ".pause" || first == ".p" || first == ".tech") {
      if (PauseStateGet().paused) {
        SendToChat("Ready Up: match is already paused.");
        return;
      }
      if (!EnqueueServerCommand("mp_pause_match")) {
        SendToChat("Ready Up: pause unavailable yet.");
        return;
      }
      PauseStateOnPaused();
      const auto ms = MatchStateGet();
      WebhookEmitMatchPaused(
          ms.map_number,
          WebhookPlayer{steamid64, playerName, ctx->roster_team[steamid64]},
          /*is_tactical=*/false,
          /*is_admin=*/false,
          /*pause_time=*/0);
      SendToChat("Ready Up: pause requested.");
      return;
    }
    if (first == ".unpause" || first == ".up") {
      if (!PauseStateGet().paused) {
        SendToChat("Ready Up: match is not paused.");
        return;
      }
      const auto ms = MatchStateGet();
      auto snap = PauseStateRequestUnpause(ctx->roster_team[steamid64]);
      // dev_bots_ready: a team with no connected human on the roster is all bots
      // (or empty); nobody can confirm for it, so confirm on its behalf. Covers
      // tactical and tech pauses alike (both go through this confirmation).
      if (DevBotsReadyEnabled()) {
        std::unordered_set<uint64_t> connected;
        for (const auto& h : ListHumans()) connected.insert(h.steamid64);
        auto botOnly = [&](WebhookTeam team) {
          for (const auto& kv : ctx->roster_team) {
            if (kv.second == team && connected.count(kv.first)) return false;
          }
          return true;
        };
        const std::pair<WebhookTeam, bool> teams[] = {{WebhookTeam::Team1, snap.team1_ready_to_unpause},
                                                      {WebhookTeam::Team2, snap.team2_ready_to_unpause}};
        for (const auto& t : teams) {
          if (t.second || !botOnly(t.first)) continue;
          snap = PauseStateRequestUnpause(t.first);
          const std::string tn = (t.first == WebhookTeam::Team1) ? ctx->team1_name : ctx->team2_name;
          Print("dev_bots_ready: auto-confirmed unpause for bot-only %s\n",
                t.first == WebhookTeam::Team1 ? "team1" : "team2");
          SendToChat(("Ready Up: dev_bots_ready - unpause confirmed for bot-only team " +
                      (tn.empty() ? std::string(t.first == WebhookTeam::Team1 ? "Team1" : "Team2") : tn) + ".")
                         .c_str());
        }
      }
      const int teams_ready = (snap.team1_ready_to_unpause ? 1 : 0) + (snap.team2_ready_to_unpause ? 1 : 0);
      WebhookEmitUnpauseRequested(ms.map_number, ctx->roster_team[steamid64], teams_ready, /*teams_needed=*/2);

      if (teams_ready >= 2) {
        if (!EnqueueServerCommand("mp_unpause_match")) {
          SendToChat("Ready Up: unpause unavailable yet.");
          return;
        }
        const int dur = PauseStatePauseDurationSeconds();
        PauseStateOnUnpaused();
        WebhookEmitMatchUnpaused(ms.map_number, dur);
        SendToChat("Ready Up: unpause accepted.");
      } else {
        SendToChat("Ready Up: unpause requested (waiting for other team).");
      }
      return;
    }
    if (first == ".gg") {
      WebhookEnqueueEvent(std::string("{") +
                          "\"event\":\"player_gg\"," +
                          "\"matchid\":" + std::to_string(ctx->matchid) + "," +
                          "\"player\":{" +
                          "\"steamid\":\"" + std::to_string(steamid64) + "\"," +
                          "\"name\":\"" + playerName + "\"," +
                          "\"team\":\"" + std::string(ctx->roster_team[steamid64] == WebhookTeam::Team1 ? "team1"
                                                       : (ctx->roster_team[steamid64] == WebhookTeam::Team2 ? "team2"
                                                                                                            : "unknown")) +
                          "\"" +
                          "}" +
                          "}");
      SendToChat("Ready Up: gg noted.");
      return;
    }
    if (first == ".ff" || first == ".forfeit") {
      const WebhookTeam team = ctx->roster_team[steamid64];
      const uint64_t want = (team == WebhookTeam::Team1) ? ctx->team1_captain_steamid64
                             : (team == WebhookTeam::Team2) ? ctx->team2_captain_steamid64
                                                            : 0ull;
      if (want == 0) {
        SendToChat("Ready Up: forfeit unavailable (captain not configured).");
        return;
      }
      if (steamid64 != want) {
        SendToChat("Ready Up: only the team captain can forfeit.");
        return;
      }

      const auto ms = MatchStateGet();
      WebhookEnqueueEvent(std::string("{") +
                          "\"event\":\"match_forfeit\"," +
                          "\"matchid\":" + std::to_string(ctx->matchid) + "," +
                          "\"map_number\":" + std::to_string(ms.map_number) + "," +
                          "\"team\":\"" + std::string(team == WebhookTeam::Team1 ? "team1" : "team2") + "\"," +
                          "\"forfeit_by\":{" +
                          "\"steamid\":\"" + std::to_string(steamid64) + "\"," +
                          "\"name\":\"" + playerName + "\"," +
                          "\"team\":\"" + std::string(team == WebhookTeam::Team1 ? "team1" : "team2") + "\"" +
                          "}" +
                          "}");
      SendToChat("Ready Up: forfeit sent.");
      return;
    }

    if (first == ".stay" || first == ".switch" || first == ".swap" || first == ".ct" || first == ".t") {
      // MatchZy-style knife side pick shortcuts. Reuse the `.ru side` implementation
      // so permissions and event emission stay consistent.
      const char* choice = nullptr;
      if (first == ".stay") choice = "stay";
      else if (first == ".switch" || first == ".swap") choice = "switch";
      else if (first == ".ct") choice = "ct";
      else if (first == ".t") choice = "t";
      if (!choice) return;
      RouteChatCommand(steamid64, playerName, std::string(".ru side ") + choice);
      return;
    }
  }

  // `.ru` alone
  if (parts.size() == 1) {
    DebugLine("ru: cmd=.ru (version)");
    SendToChat((std::string("Ready Up ") + BuildVersion()).c_str());
    return;
  }

  const std::string cmd = parts[1];
  Debug("ru: cmd=%s argc=%zu\n", cmd.c_str(), parts.size() > 2 ? parts.size() - 2 : 0u);

  auto requireAdmin = [&]() -> bool {
    // Allow server console; otherwise require admin.
    if (steamid64 == 0) return true;
    if (!readyup::IsReadyUpAdmin(steamid64)) {
      SendToChat("Ready Up: not authorized");
      return false;
    }
    return true;
  };

  auto sendAdmin = [&](const std::string& msg) {
    // SendToChat() already prepends ChatPrefix(). Add AdminPrefix() inside the message.
    SendToChat((AdminPrefix() + " " + msg).c_str());
  };

  if (cmd == "plugin" || cmd == "plugins") {
    if (!requireAdmin()) return;
    const std::vector<std::string> args(parts.begin() + 2, parts.end());
    plugins::HandlePluginCommand(args, /*replyToChat=*/steamid64 != 0);
    return;
  }

  if (cmd == "version") {
    SendToChat((std::string("Ready Up ") + BuildVersion()).c_str());
    return;
  }

  if (cmd == "help") {
    SendToChat("Ready Up: players: .r .ur .pause .unpause .gg .ff .help");
    SendToChat("Ready Up: admins: .prac | .ru idle | .ru scrim | .ru mode | .ru reload | .ru admins | .ru selftest | .ru hudtest 1-7");
    SendToChat("Ready Up: anyone: .ru state (mode, roster, ready, flags)");
    SendToChat("Ready Up: practice: .bot .cbot .boost .crouchboost .nobots");
    return;
  }

  if (cmd == "hudtest") {
    // Shows a center-HTML test variant to the caller only (checks what the CS2
    // client renders: font classes, images, unicode). Sent from GameFrame.
    if (!requireAdmin()) return;
    if (steamid64 == 0) {
      PrintLine("ru hudtest: run it from in-game chat (.ru hudtest <1-7>); the panel is shown to the caller.");
      return;
    }
    if (!FeatureEnabled(Feature::ReadyHud)) {
      sendAdmin("hudtest: the ready HUD (per-client center HTML) is off on this server; see `ru selftest`.");
      return;
    }
    const int n = (parts.size() > 2) ? std::atoi(parts[2].c_str()) : 0;
    const std::string desc = ReadyHudRequestTest(steamid64, n);
    if (desc.empty()) {
      sendAdmin("usage: .ru hudtest <n> - 1 fonts, 2 images (svg+png), 3 unicode, 4 svg, 5 png, 6 wiki png, 7 configured header");
      return;
    }
    sendAdmin("hudtest " + std::to_string(n) + " for " + playerName + " (10s): " + desc);
    return;
  }

  if (cmd == "selftest") {
    if (!requireAdmin()) return;
    // Full report to the server console; the summary (+ failing items) to chat.
    const SelftestResult r = RunSelftest(/*printToConsole=*/true);
    if (steamid64 != 0) {
      constexpr size_t kMaxChatLines = 6;
      for (size_t i = 0; i < r.failures.size(); ++i) {
        if (i == kMaxChatLines) {
          SendToChat(("Ready Up selftest: ... " + std::to_string(r.failures.size() - i) + " more (see console)").c_str());
          break;
        }
        SendToChat(("Ready Up selftest FAIL: " + r.failures[i]).c_str());
      }
      SendToChat(("Ready Up " + r.summary).c_str());
    }
    return;
  }

  if (cmd == "admins") {
    std::vector<std::string> args;
    if (parts.size() > 2) args.assign(parts.begin() + 2, parts.end());
    Debug("ru: dispatch admins args=%zu\n", args.size());
    HandleAdminsCommand(steamid64, playerName, args);
    return;
  }

  if (cmd == "reload") {
    // Allow server console; otherwise require admin.
    if (steamid64 != 0 && !readyup::IsReadyUpAdmin(steamid64)) {
      SendToChat("Reload: not authorized");
      return;
    }

    std::string err;
    if (!ReloadCfg(&err)) {
      SendToChat((std::string("Reload: failed: ") + (err.empty() ? "unknown" : err)).c_str());
      return;
    }
    (void)DevBotsReadyEnabled();  // logs if the flag flipped
    sendAdmin("cfg reloaded.");
    return;
  }

  if (cmd == "prac" || cmd == "practice") {
    if (!requireAdmin()) return;
    if (GetMode() == ReadyUpMode::Practice) {
      // Toggle off: return to idle (which execs warmup baseline via ReadyUp/idle.cfg).
      ClearReadyStates();
      WebhookClearMatchContext();
      readyup::persisted_match_state::ClearActiveMatch();
      WebhookSetHeartbeatStatus("idle");
      SetModeIdle();
      // Apply idle cfg immediately so practice cvars are reverted right away.
      (void)EnqueueServerCommand("exec ReadyUp/idle.cfg");
      sendAdmin("practice mode disabled.");
      return;
    }

    ClearReadyStates();
    WebhookClearMatchContext();
    readyup::persisted_match_state::ClearActiveMatch();
    WebhookSetHeartbeatStatus("warmup");  // non-allocatable but online
    SetModePractice();
    // Apply practice cfg immediately (MatchZy behavior) so cheats/trajectory/restart take effect
    // even if Tick is delayed.
    (void)EnqueueServerCommand("exec ReadyUp/prac.cfg");
    sendAdmin("practice mode enabled.");
    return;
  }

  if (cmd == "idle") {
    if (!requireAdmin()) return;
    const bool wasScrimWarmup = (GetMode() == ReadyUpMode::ScrimWarmup);
    ClearReadyStates();
    WebhookClearMatchContext();
    readyup::persisted_match_state::ClearActiveMatch();
    WebhookSetHeartbeatStatus("idle");
    SetModeIdle();
    // Forced idle sticks: no auto scrim warmup until `.ru scrim` or a map change.
    ScrimSetAutoEnabled(false);
    if (wasScrimWarmup) {
      // Leave the paused CS2 warmup so the server really is plain CS2 again.
      const char* cmds[] = {"mp_warmup_pausetimer 0", "mp_warmup_end", "mp_buy_anywhere 0", "mp_buytime 20",
                            "mp_respawn_on_death_ct 0", "mp_respawn_on_death_t 0"};
      for (const char* c : cmds) (void)EnqueueServerCommand(c);
    }
    sendAdmin("mode set to idle (auto scrim warmup off until .ru scrim or map change).");
    return;
  }

  if (cmd == "scrim") {
    if (!requireAdmin()) return;
    ScrimSetAutoEnabled(true);
    if (WebhookGetMatchContext()) {
      sendAdmin("scrim warmup re-enabled; a match is loaded, it applies once that match ends.");
    } else if (GetMode() == ReadyUpMode::Practice) {
      sendAdmin("scrim warmup re-enabled; leave practice (.prac) to start it.");
    } else {
      sendAdmin("scrim warmup enabled (starts as soon as a player is on CT/T).");
    }
    EmitStateLog("scrim_enable");
    return;
  }

  if (cmd == "state" || cmd == "status") {
    const auto lines = BuildStateReport();
    for (const auto& l : lines) {
      Print("%s\n", l.c_str());
      if (steamid64 != 0) SendToChat(("Ready Up " + l).c_str());
    }
    EmitStateLog("query");
    return;
  }

  if (cmd == "start") {
    if (!requireAdmin()) return;
    if (!WebhookGetMatchContext()) {
      SendToChat("Ready Up: no match loaded.");
      return;
    }
    (void)ForceStartMatch();
    sendAdmin("match force-started.");
    return;
  }

  if (cmd == "pause" || cmd == "fp" || cmd == "forcepause") {
    if (!requireAdmin()) return;
    auto ctx = WebhookGetMatchContext();
    if (!ctx) {
      SendToChat("Ready Up: no match loaded.");
      return;
    }
    if (PauseStateGet().paused) {
      SendToChat("Ready Up: match is already paused.");
      return;
    }
    if (!EnqueueServerCommand("mp_pause_match")) {
      SendToChat("Ready Up: pause unavailable yet.");
      return;
    }
    PauseStateOnPaused();
    const auto ms = MatchStateGet();
    WebhookTeam team = WebhookTeam::Unknown;
    if (steamid64 != 0) {
      if (auto it = ctx->roster_team.find(steamid64); it != ctx->roster_team.end()) team = it->second;
    }
    WebhookEmitMatchPaused(ms.map_number,
                           WebhookPlayer{steamid64, steamid64 == 0 ? "Console" : playerName, team},
                           /*is_tactical=*/false,
                           /*is_admin=*/true,
                           /*pause_time=*/0);
    sendAdmin("admin pause.");
    return;
  }

  if (cmd == "unpause" || cmd == "up" || cmd == "fup" || cmd == "forceunpause") {
    if (!requireAdmin()) return;
    auto ctx = WebhookGetMatchContext();
    if (!ctx) {
      SendToChat("Ready Up: no match loaded.");
      return;
    }
    if (!PauseStateGet().paused) {
      SendToChat("Ready Up: match is not paused.");
      return;
    }
    if (!EnqueueServerCommand("mp_unpause_match")) {
      SendToChat("Ready Up: unpause unavailable yet.");
      return;
    }
    const int dur = PauseStatePauseDurationSeconds();
    PauseStateOnUnpaused();
    const auto ms = MatchStateGet();
    WebhookEmitMatchUnpaused(ms.map_number, dur);
    sendAdmin("admin unpause.");
    return;
  }

  if (cmd == "restart") {
    if (!requireAdmin()) return;
    if (!WebhookGetMatchContext()) {
      SendToChat("Ready Up: no match loaded.");
      return;
    }
    (void)RestartMatch();
    sendAdmin("match restarted (back to warmup).");
    return;
  }

  if (cmd == "end") {
    if (!requireAdmin()) return;
    auto ctx = WebhookGetMatchContext();
    if (!ctx) {
      SendToChat("Ready Up: no match loaded.");
      return;
    }
    // Treat as a draw (winner=none) and clear context so allocator can reclaim server.
    WebhookEmitSeriesEnd(/*team1_series_score=*/0, /*team2_series_score=*/0, /*winner=*/"none", /*time_until_restore=*/0);
    WebhookClearMatchContext();
    (void)EndMatchResetServer();
    sendAdmin("match ended (forced).");
    return;
  }

  if (cmd == "recover") {
    if (!requireAdmin()) return;
    auto ctx = WebhookGetMatchContext();
    if (!ctx) {
      SendToChat("Ready Up: no match loaded.");
      return;
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
    sendAdmin(round > 0 ? "recovery requested (rewind)." : "recovery requested.");
    return;
  }

  if (cmd == "side") {
    auto ctx = WebhookGetMatchContext();
    if (!ctx) {
      SendToChat("Ready Up: no match loaded.");
      return;
    }
    if (!KnifeIsAwaitingPick()) {
      SendToChat("Ready Up: no knife side pick pending.");
      return;
    }
    if (parts.size() < 3) {
      SendToChat("Ready Up: usage: .ru side stay|switch|ct|t");
      return;
    }

    const std::string choice = parts[2];

    auto isAdminOverride = [&]() -> bool {
      if (steamid64 == 0) return true;  // server console / RCON
      return readyup::IsReadyUpAdmin(steamid64);
    };

    const bool admin = isAdminOverride();
    if (!admin && steamid64 != 0) {
      // Any player of the knife-winning team (MatchZy behavior) may pick.
      const char* winner = KnifeWinnerTeamString();
      WebhookTeam want = WebhookTeam::Unknown;
      if (std::strcmp(winner, "team1") == 0) want = WebhookTeam::Team1;
      else if (std::strcmp(winner, "team2") == 0) want = WebhookTeam::Team2;
      if (want == WebhookTeam::Unknown) {
        SendToChat("Ready Up: knife winner not known yet.");
        return;
      }
      auto it = ctx->roster_team.find(steamid64);
      if (it == ctx->roster_team.end() || it->second != want) {
        SendToChat("Ready Up: only the knife-winning team can pick sides.");
        return;
      }
    }

    if (!KnifeApplySideChoice(choice, steamid64, playerName, /*isAdminOverride=*/admin)) {
      SendToChat("Ready Up: invalid choice. Use: .ru side stay|switch|ct|t");
      return;
    }
    return;
  }

  // Unknown `ru` command; ignore to avoid chat spam.
  Debug("ru: unknown subcommand \"%s\" ignored\n", cmd.c_str());
}

}  // namespace readyup

