// readyup-match chat and `ru` commands (see match_router.h). Moved from the core's
// ru_router.cpp (player commands + match `.ru` commands) and command_buffer_hook.cpp. `ru`
// commands are main commands with subcommands (ru_commands.h), the same in chat and on the
// console. The core already
// deduped the chat line and checked that chat commands are available on this build.
#include "readyup/match_router.h"

#include "readyup/admin_check.h"
#include "readyup/config.h"
#include "readyup/engine.h"
#include "readyup/esports.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/map_names.h"
#include "readyup/match_console.h"
#include "readyup/match_events.h"
#include "readyup/match_features.h"
#include "readyup/match_signals.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/pause_state.h"
#include "readyup/persisted_match_state.h"
#include "readyup/practice_iface.h"
#include "readyup/ru_commands.h"
#include "readyup/players.h"
#include "readyup/ready_hud.h"
#include "readyup/scrim_flow.h"
#include "readyup/votes.h"
#include "readyup/weapon_cleanup.h"
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

const ru_practice_v1* Practice() {
  const ru_api* a = host::Api();
  if (!a) return nullptr;
  const auto* p = static_cast<const ru_practice_v1*>(a->get_interface(a->self, RU_PRACTICE_IFACE_NAME, 1));
  return p && p->active && p->set_active && p->help ? p : nullptr;
}

void SendAdmin(const std::string& msg) {
  // SendToChat() adds the chat prefix; the admin prefix goes inside the message.
  SendToChat((AdminPrefix() + " " + msg).c_str());
}

}  // namespace

bool MatchSetPractice(bool on) {
  if (on) {
    if (WebhookGetMatchContext()) return false;  // a loaded match keeps its own flow
    if (GetMode() == ReadyUpMode::Practice) return true;
    ClearReadyStates();
    persisted_match_state::ClearActiveMatch();
    WebhookSetHeartbeatStatus("warmup");  // non-allocatable but online
    SetModePractice();
    return true;
  }
  if (GetMode() != ReadyUpMode::Practice) return true;
  ClearReadyStates();
  WebhookSetHeartbeatStatus("idle");
  SetModeIdle();
  return true;
}

const std::vector<std::string>& MatchPlayerChatCommands() {
  static const std::vector<std::string> k = {
      ".r",    ".ready", ".unready", ".ur",   ".notready", ".nr",    ".pause", ".p",     ".tech",
      ".tac",  ".forceready", ".forcepause", ".fp", ".forceunpause", ".fup",
      ".unpause", ".up", ".gg",      ".ff",   ".forfeit",  ".stay",  ".switch", ".swap", ".ct",
      ".t",    ".help",  ".stop"};
  return k;
}

const std::vector<std::string>& MatchRuSubcommands() {
  // Main commands only (ru_commands.h); their subcommands are parsed here.
  static const std::vector<std::string> k = [] {
    std::vector<std::string> v;
    for (const auto& m : MatchRuCommands()) v.push_back(m.name);
    return v;
  }();
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
      if (const ru_practice_v1* p = Practice()) SendToChat(p->help());
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
    if (IsReadyUpAdmin(steamid64)) SendToChat("Ready Up admin: .ru help (commands) | .ru help <command> (its subcommands)");
    return;
  }

  if (first == ".forcepause" || first == ".fp") {
    MatchRuCommand(steamid64, playerName, ".ru match pause", -1);
    return;
  }
  if (first == ".forceunpause" || first == ".fup") {
    MatchRuCommand(steamid64, playerName, ".ru match unpause", -1);
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
      SendToChat("Ready Up: scrim warmup is off (admin: .ru mode scrim to enable).");
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
    // MatchZy-style knife side pick shortcuts, through `.ru match side` (same permissions + events).
    const char* choice = first == ".stay" ? "stay" : (first == ".ct") ? "ct" : (first == ".t") ? "t" : "switch";
    MatchRuCommand(steamid64, playerName, std::string(".ru match side ") + choice, -1);
    return;
  }
}

void MatchRuCommand(uint64_t steamid64, const std::string& playerName, const std::string& text, int slot) {
  const auto parts = SplitWS(text);
  if (parts.size() < 2) return;
  const std::string main = Lower(parts[1]);
  const std::string sub = parts.size() > 2 ? Lower(parts[2]) : std::string();
  const std::vector<std::string> args(parts.begin() + std::min<size_t>(parts.size(), 3), parts.end());
  const std::string who = steamid64 == 0 ? std::string("Console") : playerName;
  Debug("ru: match cmd=%s sub=%s argc=%zu\n", main.c_str(), sub.c_str(), args.size());

  // Answers (help, state, errors) go to the sender only, one chat line each; announcements that
  // concern everyone (a pause, a map change) go through sendAdmin.
  auto replyPrivate = [&](std::string msg) {
    if (msg.rfind("Ready Up: ", 0) == 0) msg.erase(0, 10);
    if (steamid64 == 0) PrintLine(msg.c_str());
    else if (slot < 0 || !ClientPrintChat(slot, (" \x04[ReadyUp]\x01 " + msg).c_str())) SendToChat(msg.c_str());
  };
  auto requireAdmin = [&]() -> bool {
    if (steamid64 == 0) return true;  // server console
    if (!IsReadyUpAdmin(steamid64)) {
      replyPrivate("not authorized");
      return false;
    }
    return true;
  };
  auto sendAdmin = [&](const std::string& msg) {
    if (steamid64 == 0) PrintLine(msg.c_str());
    else SendAdmin(msg);
  };

  const RuMainCommand* mc = FindRuMain(main);
  if (!mc) return;  // not ours (only registered names reach this)
  if (sub.empty() || sub == "help") {
    for (const auto& l : RuHelpLines(*mc)) replyPrivate(l);
    if (main == "mode") replyPrivate(std::string("mode: ") + GetModeString());
    return;
  }
  const RuSubcommand* sc = FindRuSub(*mc, sub);
  if (!sc) {
    replyPrivate(RuUnknownSubReply(main, sub));
    return;
  }
  if (sc->admin && !requireAdmin()) return;

  // ---- .ru hud -------------------------------------------------------------------------------
  if (main == "hud") {
    // Center-HTML test variant to the caller only (what the CS2 client renders).
    if (steamid64 == 0) {
      PrintLine("ru hud test: run it from in-game chat (.ru hud test <1-11>); the panel is shown to the caller.");
      return;
    }
    if (!FeatureEnabled(Feature::ReadyHud)) {
      sendAdmin("hud test: the ready HUD (per-client center HTML) is off on this server; see `ru selftest`.");
      return;
    }
    const int n = args.empty() ? 0 : std::atoi(args[0].c_str());
    const std::string desc = ReadyHudRequestTest(steamid64, n);
    if (desc.empty()) {
      sendAdmin("usage: .ru hud test <n> - 1 fonts, 2 images, 3 unicode, 4 svg, 5 png, 6 wiki png, 7 header, 8 height ruler, 9 width ruler, 10-11 image sizes");
      return;
    }
    sendAdmin("hud test " + std::to_string(n) + " for " + playerName + " (10s): " + desc);
    return;
  }

  // ---- .ru mode -----------------------------------------------------------------------------
  if (main == "mode") {
    if (sub == "show") {
      replyPrivate(std::string("mode: ") + GetModeString());
    } else if (sub == "practice") {
      // Practice mode and its tools are the practice plugin's (plugins/practice): it asks back
      // through readyup.match.v1 set_practice (MatchSetPractice).
      const ru_practice_v1* p = Practice();
      if (!p) {
        replyPrivate("Ready Up: practice mode needs the practice plugin (practice.so), which is not loaded.");
        return;
      }
      const bool on = p->active() == 0;
      const char* why = "";
      if (p->set_active(on ? 1 : 0, &why) != 1) {
        replyPrivate(std::string("Ready Up: practice mode refused: ") + (why ? why : "") + ".");
        return;
      }
      sendAdmin(on ? "practice mode enabled." : "practice mode disabled.");
      return;
    } else if (sub == "idle") {
      const bool wasScrimWarmup = (GetMode() == ReadyUpMode::ScrimWarmup);
      const bool wasPractice = (GetMode() == ReadyUpMode::Practice);
      ClearReadyStates();
      WebhookClearMatchContext();
      persisted_match_state::ClearActiveMatch();
      WebhookSetHeartbeatStatus("idle");
      SetModeIdle();
      // Forced idle sticks: no auto scrim warmup until `.ru mode scrim` or a map change.
      ScrimSetAutoEnabled(false);
      if (wasScrimWarmup) {
        // Leave the paused CS2 warmup so the server really is plain CS2 again.
        const char* cmds[] = {"mp_warmup_pausetimer 0", "mp_warmup_end", "mp_buy_anywhere 0", "mp_buytime 20",
                              "mp_respawn_on_death_ct 0", "mp_respawn_on_death_t 0"};
        for (const char* c : cmds) (void)EnqueueServerCommand(c);
        for (const char* c : kLiveDropCmds) (void)EnqueueServerCommand(c);  // weapon_cleanup.h
      }
      if (wasPractice) {
        // Leaving practice: the practice plugin puts the idle cvars back and respawns everyone.
        if (const ru_practice_v1* p = Practice()) (void)p->set_active(0, nullptr);
      }
      sendAdmin("mode set to idle (auto scrim warmup off until .ru mode scrim or a map change).");
      return;
    } else if (sub == "scrim") {
      ScrimSetAutoEnabled(true);
      if (WebhookGetMatchContext()) sendAdmin("scrim warmup re-enabled; a match is loaded, it applies once that match ends.");
      else if (GetMode() == ReadyUpMode::Practice) sendAdmin("scrim warmup re-enabled; leave practice (.prac) to start it.");
      else sendAdmin("scrim warmup enabled (starts as soon as a player is on CT/T).");
      EmitStateLog("scrim_enable");
      return;
    }
    return;
  }

  // ---- .ru match ----------------------------------------------------------------------------
  if (sub == "load") {
    std::string url, err;
    if (!ParseMatchLoad(args, &url, &err)) {
      replyPrivate("Ready Up: " + err);
      return;
    }
    if (steamid64 != 0) sendAdmin("loading the match config (see the server console).");
    (void)LoadMatchFromUrl(url);
    return;
  }
  if (sub == "state") {
    for (const auto& l : BuildStateReport()) {
      Print("%s\n", l.c_str());
      if (steamid64 != 0) replyPrivate(l);
    }
    EmitStateLog("query");
    return;
  }
  if (sub == "rules") {
    // Effective rules: ruleset preset + overrides, and what differs (esports.h).
    for (const auto& l : EsportsRulesReport()) {
      Print("%s\n", l.c_str());
      if (steamid64 != 0) replyPrivate(l);
    }
    return;
  }
  if (sub == "side") {
    auto ctx = WebhookGetMatchContext();
    if (!ctx) {
      replyPrivate("Ready Up: no match loaded.");
      return;
    }
    if (!KnifeIsAwaitingPick()) {
      replyPrivate("Ready Up: no knife side pick pending.");
      return;
    }
    if (args.empty()) {
      replyPrivate("Ready Up: usage: .ru match side stay|switch|ct|t");
      return;
    }
    const std::string choice = args[0];
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
      replyPrivate("Ready Up: invalid choice. Use: .ru match side stay|switch|ct|t");
      return;
    }
    if (steamid64 == 0) PrintLine("side: ok");
    return;
  }
  if (sub == "tech" || sub == "tac") {
    // Admin / console on behalf of a team, with the team's limits: `ru match tech team1`.
    const std::string t = args.empty() ? std::string() : Lower(args[0]);
    const WebhookTeam team = t == "team1" ? WebhookTeam::Team1 : t == "team2" ? WebhookTeam::Team2 : WebhookTeam::Unknown;
    if (team == WebhookTeam::Unknown) {
      replyPrivate("usage: ru match " + sub + " team1|team2");
      return;
    }
    if (sub == "tech") MatchFeaturesTechPause(team, steamid64, who);
    else MatchFeaturesTacticalTimeout(team, steamid64, who);
    return;
  }
  // start / restart / end / recover / pause / unpause need a loaded match.
  if (!WebhookGetMatchContext()) {
    replyPrivate("Ready Up: no match loaded.");
    return;
  }
  if (sub == "start") {
    (void)ForceStartMatch();
    sendAdmin("match force-started.");
  } else if (sub == "restart") {
    (void)RestartMatch();
    sendAdmin("match restarted (back to warmup).");
  } else if (sub == "end") {
    // Treat as a draw (winner=none) and clear the context so the allocator can reclaim the server.
    WebhookEmitSeriesEnd(0, 0, "none", 0);
    WebhookClearMatchContext();
    (void)EndMatchResetServer();
    sendAdmin("match ended (forced).");
  } else if (sub == "recover") {
    const int round = args.empty() ? 0 : std::max(0, std::atoi(args[0].c_str()));
    WebhookEmitRecoverRequested(MatchStateGet().map_number, round);
    sendAdmin(round > 0 ? "recovery requested (rewind)." : "recovery requested.");
  } else if (sub == "pause") {
    if (PauseStateGet().paused) {
      replyPrivate("Ready Up: match is already paused.");
      return;
    }
    if (!EnqueueServerCommand("mp_pause_match")) {
      replyPrivate("Ready Up: pause unavailable yet.");
      return;
    }
    PauseStateOnPaused("admin", steamid64 == 0 ? std::string("Console") : std::to_string(steamid64));
    const auto ctx = WebhookGetMatchContext();
    WebhookTeam team = WebhookTeam::Unknown;
    if (steamid64 != 0 && ctx) {
      if (auto it = ctx->roster_team.find(steamid64); it != ctx->roster_team.end()) team = it->second;
    }
    WebhookEmitMatchPaused(MatchStateGet().map_number, WebhookPlayer{steamid64, who, team},
                           /*is_tactical=*/false, /*is_admin=*/true, /*pause_time=*/0);
    sendAdmin("admin pause.");
  } else if (sub == "unpause") {
    if (!EnqueueServerCommand("mp_unpause_match")) {
      replyPrivate("Ready Up: unpause unavailable yet.");
      return;
    }
    if (!PauseStateGet().paused) {
      // An engine pause Ready Up did not start (sv_matchpause_auto_5v5 under the valve ruleset, a
      // vote): resumed anyway.
      sendAdmin("not paused by Ready Up; sent mp_unpause_match (engine pause).");
      return;
    }
    const int dur = PauseStatePauseDurationSeconds();
    PauseStateOnUnpaused();
    WebhookEmitMatchUnpaused(MatchStateGet().map_number, dur);
    sendAdmin("admin unpause.");
  }
}

void MatchRuConsole(const std::string& line) {
  // The same handler with the console as sender (replies go to the console).
  MatchRuCommand(0, "Console", line, -1);
}

}  // namespace readyup
