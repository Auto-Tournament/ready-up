// readyup-match chat and `ru` commands (see match_router.h). Moved from the core's
// ru_router.cpp (player commands + match `.ru` subcommands) and command_buffer_hook.cpp
// (`ru mode|match|side|start|restart|end|recover|...` on the console). The core already
// deduped the chat line and checked that chat commands are available on this build.
#include "readyup/match_router.h"

#include "readyup/admin_check.h"
#include "readyup/admins.h"
#include "readyup/config.h"
#include "readyup/engine.h"
#include "readyup/logging.h"
#include "readyup/match_console.h"
#include "readyup/match_events.h"
#include "readyup/match_features.h"
#include "readyup/match_signals.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/pause_state.h"
#include "readyup/persisted_match_state.h"
#include "readyup/practice_tools.h"
#include "readyup/players.h"
#include "readyup/ready_hud.h"
#include "readyup/scrim_flow.h"
#include "readyup/votes.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace readyup {
namespace {

std::vector<std::string> SplitWS(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!cur.empty()) out.push_back(std::move(cur));
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void SendAdmin(const std::string& msg) {
  // SendToChat() adds the chat prefix; the admin prefix goes inside the message.
  SendToChat((AdminPrefix() + " " + msg).c_str());
}

// Replies of `.ru ...` commands: chat for players, the console for the console.
void Reply(uint64_t steamid64, const std::string& msg) {
  if (steamid64 != 0) SendToChat(msg.c_str());
  else PrintLine(msg.c_str());
}

}  // namespace

const std::vector<std::string>& MatchPlayerChatCommands() {
  static const std::vector<std::string> k = {
      ".r",    ".ready", ".unready", ".ur",   ".notready", ".nr",    ".pause", ".p",     ".tech",
      ".tac",  ".forceready", ".forcepause", ".fp", ".forceunpause", ".fup",
      ".unpause", ".up", ".gg",      ".ff",   ".forfeit",  ".stay",  ".switch", ".swap", ".ct",
      ".t",    ".help",  ".prac",    ".tactics", ".bot",   ".cbot",  ".crouchbot", ".boost",
      ".crouchboost", ".nobots", ".stop"};
  return k;
}

const std::vector<std::string>& MatchRuSubcommands() {
  static const std::vector<std::string> k = {
      "admins", "hudtest", "prac", "practice", "idle", "scrim", "state", "status", "mode", "match", "start",
      "pause",  "fp",      "forcepause", "unpause", "up", "fup", "forceunpause", "restart", "end", "recover", "side",
      "tech",   "tac"};
  return k;
}

void MatchChatCommand(uint64_t steamid64, const std::string& playerName, const std::string& text) {
  const auto parts = SplitWS(text);
  if (parts.empty() || steamid64 == 0) return;
  const std::string first = Lower(parts[0]);
  auto ctx = WebhookGetMatchContext();
  const bool hasMatch = static_cast<bool>(ctx);

  if (first == ".help") {
    if (GetMode() == ReadyUpMode::Practice) {
      SendToChat(PracticeToolsHelp());
      return;
    }
    SendToChat("Ready Up commands: .r / .ready / .ur (.nr) | .forceready | .tac (timeout) | .tech (.pause) | .unpause");
    if (hasMatch) {
      SendToChat("Ready Up: knife: .stay/.switch (.ct/.t) | forfeit: .ff (captain)");
    } else {
      SendToChat(Cfg().scrim_knife
                     ? "Ready Up: scrim: when everyone on CT/T is READY: 5s countdown, knife round, winners .stay/.switch, live."
                     : "Ready Up: scrim: when everyone on CT/T is READY, a 5s countdown starts and the scrim goes live.");
    }
    return;
  }

  if (first == ".forcepause" || first == ".fp") {
    MatchRuCommand(steamid64, playerName, ".ru fp");
    return;
  }
  if (first == ".forceunpause" || first == ".fup") {
    MatchRuCommand(steamid64, playerName, ".ru fup");
    return;
  }

  if (first == ".prac" || first == ".tactics") {
    // MatchZy-style alias of the admin command.
    MatchRuCommand(steamid64, playerName, ".ru prac");
    return;
  }

  // Practice bot helpers (practice_tools.cpp: the bot is placed where the caller stands).
  if (first == ".nobots" || first == ".bot" || first == ".cbot" || first == ".crouchbot" || first == ".boost" ||
      first == ".crouchboost") {
    PracticeToolsBotCommand(GameEventsSlotForSteam(steamid64).value_or(-1), steamid64, first);
    return;
  }

  // Below: match / scrim ready-state commands require team membership.
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
    if (first != ".r" && first != ".ready" && first != ".unready" && first != ".ur" && first != ".notready" &&
        first != ".nr" && first != ".forceready") {
      if (first == ".stay" || first == ".switch" || first == ".swap" || first == ".ct" || first == ".t") {
        SendToChat("Ready Up: no knife side pick pending (no match loaded).");
      } else {
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
      Debug("ru: scrim roster has %zu players, %zu spectators; sender not on CT/T\n", scrim.teamNum.size(),
            scrim.spectators.size());
      for (const auto& h : ListHumans()) {
        Debug("ru:   human userid=%d steamid64=%llu name=\"%s\" team=%d\n", h.userid,
              static_cast<unsigned long long>(h.steamid64), h.name.c_str(), h.team);
      }
      for (const auto& b : ListBots()) {
        Debug("ru:   bot userid=%d name=\"%s\" team=%d (dev_bots_ready=%d)\n", b.userid, b.name.c_str(), b.team,
              DevBotsReadyEnabled() ? 1 : 0);
      }
      SendToChat("Ready Up: join CT or T first.");
      return;
    }
  }

  struct Counts {
    int r1, r2, total, expected;
  };
  auto computeCounts = [&]() {
    Counts c{0, 0, 0, 0};
    if (hasMatch) {
      c.expected = static_cast<int>(ctx->roster_team.size());
      for (const auto& kv : ctx->roster_team) {
        if (kv.first == 0 || !IsReady(kv.first)) continue;
        c.total++;
        if (kv.second == WebhookTeam::Team1) c.r1++;
        else if (kv.second == WebhookTeam::Team2) c.r2++;
      }
    } else {
      c.expected = static_cast<int>(scrim.teamNum.size());
      for (const auto& kv : scrim.teamNum) {
        if (!IsReady(kv.first)) continue;
        c.total++;
        if (kv.second == 3) c.r1++;       // CT => team1
        else if (kv.second == 2) c.r2++;  // T  => team2
      }
    }
    return c;
  };

  auto emitReady = [&](bool nowReady) {
    const auto c = computeCounts();
    WebhookTeam team = WebhookTeam::Unknown;
    if (hasMatch) {
      team = ctx->roster_team[steamid64];
    } else {
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
    ScrimNoteReadyChanged();  // scrim countdown / go-live runs in ScrimTick
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
  if (first == ".forceready") {
    MatchFeaturesForceReady(steamid64, playerName);
    return;
  }
  const bool isPauseCmd = first == ".pause" || first == ".p" || first == ".tech" || first == ".tac" ||
                          first == ".unpause" || first == ".up";
  if (isPauseCmd && !FeatureEnabled(Feature::Pauses)) {
    SendToChat("Ready Up: pauses are unavailable on this server build (see `ru selftest`).");
    return;
  }
  if (first == ".pause" || first == ".p" || first == ".tech") {
    MatchFeaturesTechPause(ctx->roster_team[steamid64], steamid64, playerName);
    return;
  }
  if (first == ".tac") {
    MatchFeaturesTacticalTimeout(ctx->roster_team[steamid64], steamid64, playerName);
    return;
  }
  if (first == ".unpause" || first == ".up") {
    MatchFeaturesUnpause(ctx->roster_team[steamid64], steamid64, playerName);
    return;
  }
  auto teamStr = [&](WebhookTeam t) {
    return std::string(t == WebhookTeam::Team1 ? "team1" : t == WebhookTeam::Team2 ? "team2" : "unknown");
  };
  if (first == ".gg") {
    WebhookEnqueueEvent(std::string("{") + "\"event\":\"player_gg\"," + "\"matchid\":" + std::to_string(ctx->matchid) +
                        "," + "\"player\":{" + "\"steamid\":\"" + std::to_string(steamid64) + "\"," + "\"name\":\"" +
                        playerName + "\"," + "\"team\":\"" + teamStr(ctx->roster_team[steamid64]) + "\"" + "}" + "}");
    if (signals::Enabled()) {
      status::Json d = status::Json::Object();
      d["team"] = teamStr(ctx->roster_team[steamid64]);
      d["reason"] = "gg";
      d["steamid64"] = std::to_string(steamid64);
      signals::Emit("gg", std::move(d));
    }
    if (!VotesGg(steamid64, ctx->roster_team[steamid64], playerName)) SendToChat("Ready Up: gg noted.");
    return;
  }
  if (first == ".stop") {
    VotesStop(steamid64, ctx->roster_team[steamid64], playerName);
    return;
  }
  if (first == ".ff" || first == ".forfeit") {
    const WebhookTeam team = ctx->roster_team[steamid64];
    const uint64_t want = (team == WebhookTeam::Team1)   ? ctx->team1_captain_steamid64
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
    const std::string ts = team == WebhookTeam::Team1 ? "team1" : "team2";
    WebhookEnqueueEvent(std::string("{") + "\"event\":\"match_forfeit\"," + "\"matchid\":" +
                        std::to_string(ctx->matchid) + "," + "\"map_number\":" + std::to_string(ms.map_number) + "," +
                        "\"team\":\"" + ts + "\"," + "\"forfeit_by\":{" + "\"steamid\":\"" +
                        std::to_string(steamid64) + "\"," + "\"name\":\"" + playerName + "\"," + "\"team\":\"" + ts +
                        "\"" + "}" + "}");
    if (signals::Enabled()) {
      status::Json d = status::Json::Object();
      d["team"] = ts;
      d["reason"] = "captain_forfeit";
      d["steamid64"] = std::to_string(steamid64);
      signals::Emit("forfeit", std::move(d));
    }
    SendToChat("Ready Up: forfeit sent.");
    return;
  }
  if (first == ".stay" || first == ".switch" || first == ".swap" || first == ".ct" || first == ".t") {
    // MatchZy-style knife side pick shortcuts, through `.ru side` (same permissions + events).
    const char* choice = first == ".stay" ? "stay" : (first == ".ct") ? "ct" : (first == ".t") ? "t" : "switch";
    MatchRuCommand(steamid64, playerName, std::string(".ru side ") + choice);
    return;
  }
}

void MatchRuCommand(uint64_t steamid64, const std::string& playerName, const std::string& text) {
  const auto parts = SplitWS(text);
  if (parts.size() < 2) return;
  const std::string cmd = Lower(parts[1]);
  Debug("ru: match cmd=%s argc=%zu\n", cmd.c_str(), parts.size() > 2 ? parts.size() - 2 : 0u);

  auto requireAdmin = [&]() -> bool {
    if (steamid64 == 0) return true;  // server console
    if (!IsReadyUpAdmin(steamid64)) {
      SendToChat("Ready Up: not authorized");
      return false;
    }
    return true;
  };
  auto sendAdmin = [&](const std::string& msg) {
    if (steamid64 == 0) PrintLine(msg.c_str());
    else SendAdmin(msg);
  };

  if (cmd == "hudtest") {
    // Center-HTML test variant to the caller only (what the CS2 client renders).
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

  if (cmd == "admins") {
    std::vector<std::string> args;
    if (parts.size() > 2) args.assign(parts.begin() + 2, parts.end());
    HandleAdminsCommand(steamid64, playerName, args);
    return;
  }

  if (cmd == "prac" || cmd == "practice") {
    if (!requireAdmin()) return;
    if (GetMode() == ReadyUpMode::Practice) {
      // Toggle off: back to idle (idle.cfg reverts the practice cvars right away).
      ClearReadyStates();
      WebhookClearMatchContext();
      persisted_match_state::ClearActiveMatch();
      WebhookSetHeartbeatStatus("idle");
      SetModeIdle();
      (void)EnqueueServerCommand("exec ReadyUp/idle.cfg");
      sendAdmin("practice mode disabled.");
      return;
    }
    ClearReadyStates();
    WebhookClearMatchContext();
    persisted_match_state::ClearActiveMatch();
    WebhookSetHeartbeatStatus("warmup");  // non-allocatable but online
    SetModePractice();
    (void)EnqueueServerCommand("exec ReadyUp/prac.cfg");  // MatchZy behaviour: right away
    sendAdmin("practice mode enabled.");
    return;
  }

  if (cmd == "idle") {
    if (!requireAdmin()) return;
    const bool wasScrimWarmup = (GetMode() == ReadyUpMode::ScrimWarmup);
    ClearReadyStates();
    WebhookClearMatchContext();
    persisted_match_state::ClearActiveMatch();
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
    if (WebhookGetMatchContext()) sendAdmin("scrim warmup re-enabled; a match is loaded, it applies once that match ends.");
    else if (GetMode() == ReadyUpMode::Practice) sendAdmin("scrim warmup re-enabled; leave practice (.prac) to start it.");
    else sendAdmin("scrim warmup enabled (starts as soon as a player is on CT/T).");
    EmitStateLog("scrim_enable");
    return;
  }

  if (cmd == "state" || cmd == "status") {
    for (const auto& l : BuildStateReport()) {
      Print("%s\n", l.c_str());
      if (steamid64 != 0) SendToChat(("Ready Up " + l).c_str());
    }
    EmitStateLog("query");
    return;
  }

  if (cmd == "mode") {
    Reply(steamid64, std::string("mode: ") + GetModeString());
    return;
  }

  if (cmd == "start") {
    if (!requireAdmin()) return;
    if (!WebhookGetMatchContext()) {
      Reply(steamid64, "Ready Up: no match loaded.");
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
      Reply(steamid64, "Ready Up: no match loaded.");
      return;
    }
    if (PauseStateGet().paused) {
      Reply(steamid64, "Ready Up: match is already paused.");
      return;
    }
    if (!EnqueueServerCommand("mp_pause_match")) {
      Reply(steamid64, "Ready Up: pause unavailable yet.");
      return;
    }
    PauseStateOnPaused("admin", steamid64 == 0 ? std::string("Console") : std::to_string(steamid64));
    const auto ms = MatchStateGet();
    WebhookTeam team = WebhookTeam::Unknown;
    if (steamid64 != 0) {
      if (auto it = ctx->roster_team.find(steamid64); it != ctx->roster_team.end()) team = it->second;
    }
    WebhookEmitMatchPaused(ms.map_number, WebhookPlayer{steamid64, steamid64 == 0 ? "Console" : playerName, team},
                           /*is_tactical=*/false, /*is_admin=*/true, /*pause_time=*/0);
    sendAdmin("admin pause.");
    return;
  }

  if (cmd == "tech" || cmd == "tac") {
    // Admin / console on behalf of a team, with the team's limits: `ru tech team1`, `ru tac team2`.
    if (!requireAdmin()) return;
    const std::string t = parts.size() > 2 ? Lower(parts[2]) : std::string();
    const WebhookTeam team = t == "team1" ? WebhookTeam::Team1 : t == "team2" ? WebhookTeam::Team2 : WebhookTeam::Unknown;
    if (team == WebhookTeam::Unknown) {
      Reply(steamid64, "usage: ru " + cmd + " team1|team2");
      return;
    }
    if (cmd == "tech") MatchFeaturesTechPause(team, steamid64, steamid64 == 0 ? "Console" : playerName);
    else MatchFeaturesTacticalTimeout(team, steamid64, steamid64 == 0 ? "Console" : playerName);
    return;
  }

  if (cmd == "unpause" || cmd == "up" || cmd == "fup" || cmd == "forceunpause") {
    if (!requireAdmin()) return;
    if (!WebhookGetMatchContext()) {
      Reply(steamid64, "Ready Up: no match loaded.");
      return;
    }
    if (!PauseStateGet().paused) {
      Reply(steamid64, "Ready Up: match is not paused.");
      return;
    }
    if (!EnqueueServerCommand("mp_unpause_match")) {
      Reply(steamid64, "Ready Up: unpause unavailable yet.");
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
      Reply(steamid64, "Ready Up: no match loaded.");
      return;
    }
    (void)RestartMatch();
    sendAdmin("match restarted (back to warmup).");
    return;
  }

  if (cmd == "end") {
    if (!requireAdmin()) return;
    if (!WebhookGetMatchContext()) {
      Reply(steamid64, "Ready Up: no match loaded.");
      return;
    }
    // Treat as a draw (winner=none) and clear the context so the allocator can reclaim the server.
    WebhookEmitSeriesEnd(0, 0, "none", 0);
    WebhookClearMatchContext();
    (void)EndMatchResetServer();
    sendAdmin("match ended (forced).");
    return;
  }

  if (cmd == "recover") {
    if (!requireAdmin()) return;
    if (!WebhookGetMatchContext()) {
      Reply(steamid64, "Ready Up: no match loaded.");
      return;
    }
    int round = parts.size() >= 3 ? std::max(0, std::atoi(parts[2].c_str())) : 0;
    const auto ms = MatchStateGet();
    WebhookEmitRecoverRequested(ms.map_number, round);
    sendAdmin(round > 0 ? "recovery requested (rewind)." : "recovery requested.");
    return;
  }

  if (cmd == "side") {
    auto ctx = WebhookGetMatchContext();
    if (!ctx) {
      Reply(steamid64, "Ready Up: no match loaded.");
      return;
    }
    if (!KnifeIsAwaitingPick()) {
      Reply(steamid64, "Ready Up: no knife side pick pending.");
      return;
    }
    if (parts.size() < 3) {
      Reply(steamid64, "Ready Up: usage: .ru side stay|switch|ct|t");
      return;
    }
    const std::string choice = parts[2];
    const bool admin = steamid64 == 0 || IsReadyUpAdmin(steamid64);
    if (!admin) {
      // Any player of the knife-winning team (MatchZy behaviour) may pick.
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
    if (!KnifeApplySideChoice(choice, steamid64, steamid64 == 0 ? std::string("Console") : playerName, admin)) {
      Reply(steamid64, "Ready Up: invalid choice. Use: .ru side stay|switch|ct|t");
      return;
    }
    if (steamid64 == 0) PrintLine("side: ok");
    return;
  }

  if (cmd == "match") {
    if (steamid64 != 0) {
      SendToChat("Ready Up: ru match load runs from the server console / RCON.");
      return;
    }
    MatchRuConsole(text);
    return;
  }

  Debug("ru: unknown match subcommand \"%s\" ignored\n", cmd.c_str());
}

void MatchRuConsole(const std::string& line) {
  const auto parts = SplitWS(line);
  if (parts.size() < 2) return;
  const std::string sub = Lower(parts[1]);

  if (sub == "mode") {
    if (parts.size() == 2) {
      Print("mode: %s\n", GetModeString());
      return;
    }
    const std::string m = Lower(parts[2]);
    if (m == "idle") return MatchRuCommand(0, "Console", ".ru idle");
    if (m == "practice") return MatchRuCommand(0, "Console", ".ru practice");
    PrintLine("Usage: ru mode [idle|practice]");
    return;
  }

  if (sub == "match") {
    if (parts.size() >= 3 && Lower(parts[2]) == "load") {
      if (parts.size() < 4) {
        PrintLine("Usage: ru match load <url>");
        return;
      }
      (void)LoadMatchFromUrl(parts[3]);
      return;
    }
    PrintLine("Usage: ru match load <url>");
    return;
  }

  if (sub == "start" || sub == "restart" || sub == "end" || sub == "recover") {
    if (!WebhookGetMatchContext()) {
      Print("%s: no match loaded\n", sub.c_str());
      return;
    }
    if (sub == "start") {
      (void)ForceStartMatch();
    } else if (sub == "restart") {
      (void)RestartMatch();
    } else if (sub == "end") {
      WebhookEmitSeriesEnd(0, 0, "none", 0);
      WebhookClearMatchContext();
      (void)EndMatchResetServer();
    } else {
      const int round = parts.size() >= 3 ? std::max(0, std::atoi(parts[2].c_str())) : 0;
      WebhookEmitRecoverRequested(MatchStateGet().map_number, round);
    }
    Print("%s: ok\n", sub.c_str());
    return;
  }

  if (sub == "side") {
    if (!WebhookGetMatchContext()) {
      PrintLine("side: no match loaded");
      return;
    }
    if (!KnifeIsAwaitingPick()) {
      PrintLine("side: no knife side pick pending");
      return;
    }
    if (parts.size() < 3) {
      PrintLine("Usage: ru side <stay|switch|ct|t>");
      return;
    }
    if (!KnifeApplySideChoice(parts[2], 0, "Console", /*isAdminOverride=*/true)) {
      PrintLine("side: invalid choice (use: stay|switch|ct|t)");
      return;
    }
    PrintLine("side: ok");
    return;
  }

  // idle / practice / prac / scrim / state / status / admins / pause / unpause / hudtest ...:
  // the chat handler with the console as sender (as `ru idle` always did).
  std::string rest = ".ru";
  for (size_t i = 1; i < parts.size(); ++i) rest += " " + parts[i];
  MatchRuCommand(0, "Console", rest);
}

}  // namespace readyup
