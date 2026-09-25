#include "readyup/scrim_flow.h"

#include "readyup/chat.h"
#include "readyup/client_print.h"
#include "readyup/config.h"
#include "readyup/disabled.h"
#include "readyup/game_events.h"
#include "readyup/logging.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/ready_hud.h"
#include "readyup/slot_registry.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace readyup {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kTickInterval = std::chrono::milliseconds(250);
constexpr auto kReminderInterval = std::chrono::seconds(30);
constexpr auto kPeriodicStateLog = std::chrono::seconds(30);
constexpr auto kNoTeamGrace = std::chrono::seconds(5);
constexpr auto kEmptyScrimTimeout = std::chrono::seconds(60);
constexpr int kCountdownSeconds = 5;
constexpr int kRestartSeconds = 1;

struct FlowState {
  std::recursive_mutex mu;
  bool autoEnabled = true;
  std::string lastMap;
  Clock::time_point lastRun{};

  // Scrim warmup bookkeeping.
  Clock::time_point lastReminder{};
  Clock::time_point noTeamSince{};
  Clock::time_point noHumansSince{};
  bool countdownActive = false;
  Clock::time_point countdownDeadline{};
  int countdownLastAnnounced = -1;

  // `state:` log dedupe.
  std::string lastLoggedMode;
  std::string lastSig;
  Clock::time_point lastStateLog{};
};

FlowState& F() {
  static FlowState f;
  return f;
}

static bool IsZero(Clock::time_point t) {
  return t.time_since_epoch().count() == 0;
}

static uint64_t NowMatchId() {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  return static_cast<uint64_t>(ms);
}

static const char* TeamTag(int tn) {
  if (tn == 3) return "CT";
  if (tn == 2) return "T";
  if (tn == 1) return "spec";
  return "none";
}

static std::string NameFor(uint64_t steamid64) {
  for (const auto& h : ListHumans()) {
    if (h.steamid64 == steamid64 && !h.name.empty()) return h.name;
  }
  return std::to_string(static_cast<unsigned long long>(steamid64));
}

// Cross-check only: the human table (log lines + player_team events, latest
// wins) is the answer. When engine events are up and the player's engine slot
// is known, compare with the controller's m_iTeamNum and log a debug line when
// they disagree (once per distinct disagreement).
static void CrossCheckTeam(const HumanIdentity& h) {
  if (!DebugEnabled() || !GameEventsListenerInstalled() || h.slot < 0) return;
  const auto netvar = GetCsTeamNumForSlot(h.slot);
  static std::mutex s_mu;
  static std::unordered_map<uint64_t, std::string> s_last;
  std::string sig;
  if (netvar && *netvar != h.team) {
    sig = std::to_string(h.team) + "/" + std::to_string(*netvar) + "/" + std::to_string(h.slot);
  }
  std::lock_guard<std::mutex> lk(s_mu);
  std::string& last = s_last[h.steamid64];
  if (sig == last) return;
  last = sig;
  if (sig.empty()) return;
  const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - h.teamAt).count();
  Debug("teams: sources disagree for %s steamid64=%llu: table=%s (from %s, %lldms ago, log userid=%d) vs "
        "m_iTeamNum[slot %d]=%s; using the table\n",
        h.name.c_str(), static_cast<unsigned long long>(h.steamid64), TeamTag(h.team), TeamSourceName(h.teamSource),
        static_cast<long long>(ageMs), h.userid, h.slot, TeamTag(*netvar));
}

// dev_bots_scrim: nobody human on CT/T, but bots on both sides.
static bool BotsOnlyScrim(const ScrimCounts& c) {
  return c.total == 0 && c.devBotsCt > 0 && c.devBotsT > 0 && DevBotsScrimEnabled();
}

static bool IsWarmupMode(ReadyUpMode m) {
  return m == ReadyUpMode::ScrimWarmup || m == ReadyUpMode::MatchWarmup;
}

// Ready counts for the active match context (roster-based).
static void MatchReadyCounts(const WebhookMatchContext& ctx, int& ready, int& total) {
  ready = 0;
  total = 0;
  for (const auto& kv : ctx.roster_team) {
    if (kv.first == 0) continue;
    total++;
    if (IsReady(kv.first)) ready++;
  }
}

static std::string MatchLabel(const std::optional<WebhookMatchContext>& ctx) {
  if (!ctx) return "none";
  const std::string slug = ctx->slug.empty() ? "match" : ctx->slug;
  return slug + ":" + std::to_string(static_cast<unsigned long long>(ctx->matchid));
}

// Field part of the `state:` line (no reason). Also used as the change signature.
static std::string BuildStateFieldsLocked(FlowState& f) {
  const ReadyUpMode mode = GetMode();
  const auto ctx = WebhookGetMatchContext();
  const auto roster = BuildScrimRoster();
  const auto c = CountScrimRoster(roster);

  int botsCt = 0, botsT = 0;
  for (const auto& b : ListBots()) {
    if (b.team == 3) botsCt++;
    else if (b.team == 2) botsT++;
  }

  int ready = c.ready, total = c.total;
  if (ctx) MatchReadyCounts(*ctx, ready, total);

  std::string s;
  s.reserve(256);
  s += "mode=";
  s += GetModeString();
  s += " warmup=";
  s += IsWarmupMode(mode) ? "1" : "0";
  s += " ct=" + std::to_string(c.humansCt) + "+" + std::to_string(botsCt);
  s += " t=" + std::to_string(c.humansT) + "+" + std::to_string(botsT);
  s += " spec=" + std::to_string(roster.spectators.size());
  s += " ready=" + std::to_string(ready) + "/" + std::to_string(total);
  s += " match=" + MatchLabel(ctx);
  s += " events=";
  s += GameEventsListenerInstalled() ? "1" : "0";
  s += " map=";
  const auto ms = MatchStateGet();
  s += ms.current_map.empty() ? "?" : ms.current_map;
  s += " scrim_auto=";
  s += f.autoEnabled ? "1" : "0";
  s += " dev_bots_ready=";
  s += DevBotsReadyEnabled() ? "1" : "0";
  s += " dev_bots_scrim=";
  s += DevBotsScrimEnabled() ? "1" : "0";
  if (f.countdownActive) {
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(f.countdownDeadline - Clock::now()).count();
    s += " countdown=" + std::to_string(std::max<long long>(0, left + 1));
  }
  if (mode == ReadyUpMode::MatchWarmup && GoLiveTriggered()) s += " golive=pending";
  if (const char* kp = KnifePhaseString()) {
    s += " knife=";
    s += kp;
  }
  return s;
}

static void EmitStateLogLocked(FlowState& f, const char* reason) {
  const std::string fields = BuildStateFieldsLocked(f);
  Print("state: %s reason=%s\n", fields.c_str(), reason ? reason : "-");
  f.lastSig = fields;
  f.lastLoggedMode = GetModeString();
  f.lastStateLog = Clock::now();
}

static std::string ReadyProgress(const ScrimCounts& c) {
  return "(" + std::to_string(c.ready) + "/" + std::to_string(c.total) + " ready)";
}

static std::string NotReadyNames(const ScrimRoster& roster, int maxNames) {
  std::string list;
  int n = 0;
  for (const auto& kv : roster.teamNum) {
    if (IsReady(kv.first)) continue;
    if (n >= maxNames) {
      list += ", ...";
      break;
    }
    if (!list.empty()) list += ", ";
    list += NameFor(kv.first);
    n++;
  }
  return list;
}

static void SendReminder(const ScrimRoster& roster, const ScrimCounts& c) {
  if (HudReplacesChat()) return;  // the ready panel shows this
  std::string msg = "Ready Up: type .r when ready " + ReadyProgress(c);
  const std::string waiting = NotReadyNames(roster, 5);
  if (!waiting.empty()) msg += " - waiting: " + waiting;
  SendToChat(msg.c_str());
  if (!c.bothSides) {
    SendToChat(DevBotsReadyEnabled() ? "Ready Up: need players (or bots) on both CT and T to start."
                                     : "Ready Up: need players on both CT and T to start.");
  }
}

static void CancelCountdownLocked(FlowState& f) {
  f.countdownActive = false;
  f.countdownDeadline = {};
  f.countdownLastAnnounced = -1;
}

static void EndEmptyScrim() {
  PrintLine("scrim: no humans connected for 60s; ending scrim match and returning to idle.");
  WebhookEmitSeriesEnd(/*team1_series_score=*/0, /*team2_series_score=*/0, /*winner=*/"none",
                       /*time_until_restore=*/0);
  WebhookClearMatchContext();
  ClearReadyStates();
  (void)EndMatchResetServer();
}

static void ScrimWarmupStepLocked(FlowState& f, Clock::time_point now, const ScrimRoster& roster,
                                  const ScrimCounts& c) {
  if (c.total == 0 && !BotsOnlyScrim(c)) {
    CancelCountdownLocked(f);
    if (IsZero(f.noTeamSince)) f.noTeamSince = now;
    if ((now - f.noTeamSince) >= kNoTeamGrace) {
      f.noTeamSince = {};
      ClearReadyStates();
      SetModeIdle();
      PrintLine("scrim: no humans on CT/T; back to idle.");
    }
    return;
  }
  f.noTeamSince = {};

  if (f.countdownActive) {
    if (!c.allReady || !c.bothSides) {
      CancelCountdownLocked(f);
      if (!HudReplacesChat()) SendToChat(("Ready Up: countdown cancelled " + ReadyProgress(c) + ".").c_str());
      f.lastReminder = now;
      return;
    }
    const auto leftMs = std::chrono::duration_cast<std::chrono::milliseconds>(f.countdownDeadline - now).count();
    if (leftMs <= 0) {
      CancelCountdownLocked(f);
      if (!MaybeStartScrimIfAllReady(roster)) {
        SendToChat("Ready Up: could not start the scrim; still in warmup.");
      }
      return;
    }
    const int secs = static_cast<int>((leftMs + 999) / 1000);
    if (secs <= 3 && secs != f.countdownLastAnnounced) {
      f.countdownLastAnnounced = secs;
      if (!HudReplacesChat()) SendToChat(((Cfg().scrim_knife ? "Ready Up: knife round in " : "Ready Up: going live in ") + std::to_string(secs) + "...").c_str());
    }
    return;
  }

  if (c.allReady && c.bothSides) {
    f.countdownActive = true;
    f.countdownDeadline = now + std::chrono::seconds(kCountdownSeconds);
    f.countdownLastAnnounced = kCountdownSeconds;
    if (BotsOnlyScrim(c)) {
      Print("dev_bots_scrim: bots-only scrim ready (CT %d bot(s), T %d bot(s)); %s in %ds\n", c.devBotsCt,
            c.devBotsT, Cfg().scrim_knife ? "knife round" : "going live", kCountdownSeconds);
    }
    if (!HudReplacesChat()) SendToChat(("Ready Up: all " + std::to_string(c.total) + " player(s) ready - " + (Cfg().scrim_knife ? "knife round" : "going live") + " in " +
                std::to_string(kCountdownSeconds) + "s (.ur to cancel).")
                   .c_str());
    return;
  }

  // The ready HUD is the ready-up UI (who is ready, `.r` hint, both-sides
  // note); chat reminders only when it is not on screen.
  if (IsZero(f.lastReminder) || (now - f.lastReminder) >= kReminderInterval) {
    f.lastReminder = now;
    if (!ReadyHudShowing()) SendReminder(roster, c);
  }
}

}  // namespace

ScrimRoster BuildScrimRoster() {
  ScrimRoster out;
  for (const auto& h : ListHumans()) {
    if (h.steamid64 == 0) continue;
    CrossCheckTeam(h);
    const int tn = h.team;
    if (tn == 2 || tn == 3) out.teamNum[h.steamid64] = tn;
    else out.spectators.insert(h.steamid64);
  }
  if (DevBotsReadyEnabled() || DevBotsScrimEnabled()) {
    for (const auto& b : ListBots()) {
      if (b.team == 2 || b.team == 3) out.devBots[b.pseudo_id] = b.team;
    }
  }
  return out;
}

ScrimCounts CountScrimRoster(const ScrimRoster& roster) {
  ScrimCounts c;
  for (const auto& kv : roster.teamNum) {
    if (kv.second == 3) c.humansCt++;
    else if (kv.second == 2) c.humansT++;
    if (IsReady(kv.first)) c.ready++;
  }
  for (const auto& kv : roster.devBots) {
    if (kv.second == 3) c.devBotsCt++;
    else if (kv.second == 2) c.devBotsT++;
  }
  c.total = c.humansCt + c.humansT;
  c.bothSides = (c.humansCt + c.devBotsCt) > 0 && (c.humansT + c.devBotsT) > 0;
  c.allReady = c.total > 0 && c.ready == c.total;
  // dev_bots_scrim: no humans on CT/T and bots on both sides = everyone (the bots) ready.
  if (BotsOnlyScrim(c)) c.allReady = true;
  return c;
}

bool MaybeStartScrimIfAllReady(const ScrimRoster& roster) {
  if (WebhookGetMatchContext()) return false;
  if (GetMode() != ReadyUpMode::ScrimWarmup) return false;
  // No humans on CT/T: only dev_bots_scrim may start a scrim (bots on both sides).
  if (roster.teamNum.empty() && (roster.devBots.empty() || !DevBotsScrimEnabled())) return false;

  const auto c = CountScrimRoster(roster);
  if (!roster.devBots.empty()) {
    Debug("ru: scrim check humans ct=%d t=%d allReady=%d; dev bots (always ready) ct=%d t=%d\n", c.humansCt,
          c.humansT, c.allReady ? 1 : 0, c.devBotsCt, c.devBotsT);
  }
  // Need players on both sides to start a scrim.
  if (!c.bothSides) return false;
  if (!c.allReady) return false;

  const auto ms = MatchStateGet();

  WebhookMatchContext ctx;
  ctx.matchid = NowMatchId();
  ctx.slug = "scrim";
  ctx.num_maps = 1;
  ctx.team1_name = "Team1";
  ctx.team2_name = "Team2";
  // Defaults: MR12 + OT (MatchZy-style baseline).
  ctx.maxRounds = 24;
  ctx.overtime_enabled = true;
  ctx.overtimeSegments = 3;
  // Scrim uses current teams; team1=CT, team2=T. With scrim_knife the side is
  // decided by the knife round (map side becomes team1_ct/team2_ct after the pick).
  const bool knife = Cfg().scrim_knife;
  ctx.map_sides = {knife ? "knife" : "team1_ct"};
  ctx.knifeDecisionSeconds = std::max(5, std::min(300, Cfg().knife_pick_seconds));
  if (!ms.current_map.empty()) ctx.maplist = {ms.current_map};

  for (const auto& kv : roster.teamNum) {
    const uint64_t sid = kv.first;
    if (IsDevBotId(sid)) continue;  // never let bot pseudo-ids into the match context
    if (kv.second == 3) ctx.roster_team[sid] = WebhookTeam::Team1;
    else if (kv.second == 2) ctx.roster_team[sid] = WebhookTeam::Team2;
  }
  ctx.spectators = roster.spectators;

  // Snapshot ready states, then let modes reset match state, then restore.
  std::unordered_set<uint64_t> readyBefore;
  for (const auto& kv : roster.teamNum) {
    if (IsReady(kv.first)) readyBefore.insert(kv.first);
  }

  WebhookSetMatchContext(std::move(ctx));
  OnMatchLoaded();
  WebhookEmitSeriesStart();
  WebhookSetHeartbeatStatus("warmup");

  // Restore readiness (MatchZy scrim behavior).
  for (uint64_t sid : readyBefore) {
    if (!IsReady(sid)) (void)SetReady(sid, true);
  }

  if (roster.teamNum.empty()) {
    Print("dev_bots_scrim is ON — bots-only scrim started with %d CT + %d T bot(s), no humans "
          "(bots are not on the match roster).\n",
          c.devBotsCt, c.devBotsT);
  } else if (!roster.devBots.empty()) {
    Print("dev_bots_ready is ON — bots count as ready: scrim started with %d CT + %d T bot(s) "
          "(bots are not on the match roster).\n",
          c.devBotsCt, c.devBotsT);
  }
  if (knife) {
    // Log-driven knife round (modes.cpp); LIVE after the side pick.
    if (!StartKnifeRound()) {
      PrintLine("scrim: knife round could not start (command buffer?); going live without it.");
      (void)WebhookUpdateMapSide(1, "team1_ct");
      const bool goLive = ScrimGoLive(kRestartSeconds);
      SendToChat(goLive ? "Ready Up: scrim starting - restarting the game, LIVE after the restart."
                        : "Ready Up: scrim created but the go-live commands could not be queued.");
    }
    return true;
  }
  const bool goLive = ScrimGoLive(kRestartSeconds);
  SendToChat(goLive ? "Ready Up: scrim starting - restarting the game, LIVE after the restart."
                    : "Ready Up: scrim created but the go-live commands could not be queued.");
  return true;
}

void ScrimSetAutoEnabled(bool enabled) {
  auto& f = F();
  std::lock_guard<std::recursive_mutex> lk(f.mu);
  f.autoEnabled = enabled;
  if (!enabled) CancelCountdownLocked(f);
}

bool ScrimAutoEnabled() {
  auto& f = F();
  std::lock_guard<std::recursive_mutex> lk(f.mu);
  return f.autoEnabled;
}

int ScrimCountdownSecondsLeft() {
  auto& f = F();
  std::lock_guard<std::recursive_mutex> lk(f.mu);
  if (!f.countdownActive) return -1;
  const auto leftMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(f.countdownDeadline - Clock::now()).count();
  return static_cast<int>(std::max<long long>(0, (leftMs + 999) / 1000));
}

void ScrimNoteReadyChanged() {
  auto& f = F();
  std::lock_guard<std::recursive_mutex> lk(f.mu);
  EmitStateLogLocked(f, "ready");
}

void EmitStateLog(const char* reason) {
  auto& f = F();
  std::lock_guard<std::recursive_mutex> lk(f.mu);
  EmitStateLogLocked(f, reason);
}

void ScrimTick() {
  if (IsDisabled()) return;
  auto& f = F();
  std::lock_guard<std::recursive_mutex> lk(f.mu);

  const auto now = Clock::now();
  if (!IsZero(f.lastRun) && (now - f.lastRun) < kTickInterval) return;
  f.lastRun = now;

  // Map change: re-arm auto scrim warmup (undo a previous `.ru idle`).
  {
    const auto ms = MatchStateGet();
    if (ms.current_map != f.lastMap) {
      f.lastMap = ms.current_map;
      if (!f.autoEnabled) PrintLine("scrim: map changed; auto scrim warmup re-enabled.");
      f.autoEnabled = true;
      CancelCountdownLocked(f);
    }
  }

  ReadyUpMode mode = GetMode();
  const auto ctx = WebhookGetMatchContext();
  const auto roster = BuildScrimRoster();
  const auto c = CountScrimRoster(roster);
  const size_t humansConnected = ListHumans().size();

  if (!ctx) {
    f.noHumansSince = {};
    const bool botsOnly = BotsOnlyScrim(c);
    if (mode == ReadyUpMode::Idle && f.autoEnabled && (c.total > 0 || botsOnly)) {
      if (SetModeScrimWarmup()) {
        if (botsOnly) {
          Print("dev_bots_scrim: bots-only scrim warmup (CT %d bot(s), T %d bot(s), no humans).\n", c.devBotsCt,
                c.devBotsT);
        }
        mode = ReadyUpMode::ScrimWarmup;
        WebhookSetHeartbeatStatus("idle");  // still allocatable for real matches
        CancelCountdownLocked(f);
        f.noTeamSince = {};
        f.lastReminder = now;
        if (!HudReplacesChat()) SendToChat(("Ready Up: scrim warmup - type .r when ready " + ReadyProgress(c) +
                    ". Goes live when everyone on CT/T is ready.")
                       .c_str());
      }
    }
    if (mode == ReadyUpMode::ScrimWarmup) {
      ScrimWarmupStepLocked(f, now, roster, c);
    } else {
      CancelCountdownLocked(f);
    }
  } else {
    CancelCountdownLocked(f);
    f.noTeamSince = {};
    // dev_bots_scrim: a bots-only scrim is the point, so no empty-scrim timeout.
    if (ctx->slug == "scrim" && humansConnected == 0 && !DevBotsScrimEnabled()) {
      if (IsZero(f.noHumansSince)) f.noHumansSince = now;
      if ((now - f.noHumansSince) >= kEmptyScrimTimeout) {
        f.noHumansSince = {};
        EndEmptyScrim();
      }
    } else {
      f.noHumansSince = {};
    }
  }

  // Structured state log: every mode transition, every change of the fields
  // (ready counts, teams, bots, match, countdown), and every 30s while humans
  // are connected. Quiet when the server is empty and nothing changes.
  const std::string curMode = GetModeString();
  if (curMode != f.lastLoggedMode) {
    EmitStateLogLocked(f, "mode");
    return;
  }
  const std::string sig = BuildStateFieldsLocked(f);
  // countdown=N ticks every second; only its start/stop is a state change.
  auto stripCountdown = [](std::string s) {
    const std::string key = " countdown=";
    const size_t p = s.find(key);
    if (p == std::string::npos) return s;
    const size_t v = p + key.size();
    const size_t e = s.find(' ', v);
    s.replace(v, e == std::string::npos ? std::string::npos : e - v, "*");
    return s;
  };
  if (stripCountdown(sig) != stripCountdown(f.lastSig)) {
    EmitStateLogLocked(f, "change");
    return;
  }
  if (humansConnected > 0 && (IsZero(f.lastStateLog) || (now - f.lastStateLog) >= kPeriodicStateLog)) {
    EmitStateLogLocked(f, "periodic");
  }
}

std::vector<std::string> BuildStateReport() {
  auto& f = F();
  std::lock_guard<std::recursive_mutex> lk(f.mu);

  std::vector<std::string> out;
  const ReadyUpMode mode = GetMode();
  const auto ctx = WebhookGetMatchContext();
  const auto roster = BuildScrimRoster();
  const auto c = CountScrimRoster(roster);
  const auto ms = MatchStateGet();

  out.push_back(std::string("state: mode=") + GetModeString() + " warmup=" + (IsWarmupMode(mode) ? "on" : "off") +
                " match=" + MatchLabel(ctx) + " map=" + (ms.current_map.empty() ? "?" : ms.current_map) +
                (f.countdownActive ? " (countdown running)" : "") +
                ((mode == ReadyUpMode::MatchWarmup && GoLiveTriggered()) ? " (go-live pending Round_Start)" : "") +
                (KnifePhaseString() ? std::string(" knife=") + KnifePhaseString() : std::string()));

  // Players: one entry per connected human (CT/T first, then spectators).
  std::vector<std::string> entries;
  for (const auto& h : ListHumans()) {
    const int tn = h.team;
    std::string e = (h.name.empty() ? std::to_string(static_cast<unsigned long long>(h.steamid64)) : h.name);
    e += " [";
    e += TeamTag(tn);
    e += "]";
    if (tn == 2 || tn == 3 || (ctx && ctx->roster_team.count(h.steamid64))) {
      e += IsReady(h.steamid64) ? " READY" : " not ready";
    }
    entries.push_back(std::move(e));
  }
  std::sort(entries.begin(), entries.end());
  if (entries.empty()) {
    out.push_back("players: (no humans connected)");
  } else {
    std::string line = "players:";
    for (const auto& e : entries) {
      if (line.size() + e.size() > 110) {
        out.push_back(line);
        line = "players:";
      }
      line += " " + e + ";";
    }
    out.push_back(line);
  }

  int botsCt = 0, botsT = 0;
  for (const auto& b : ListBots()) {
    if (b.team == 3) botsCt++;
    else if (b.team == 2) botsT++;
  }
  int ready = c.ready, total = c.total;
  if (ctx) MatchReadyCounts(*ctx, ready, total);
  out.push_back("ready " + std::to_string(ready) + "/" + std::to_string(total) + " | humans CT=" +
                std::to_string(c.humansCt) + " T=" + std::to_string(c.humansT) + " | bots CT=" +
                std::to_string(botsCt) + " T=" + std::to_string(botsT) +
                (DevBotsReadyEnabled() ? " (count as ready)" : "") + " | both sides: " + (c.bothSides ? "yes" : "no"));

  out.push_back(std::string("flags: dev_bots_ready=") + (DevBotsReadyEnabled() ? "1" : "0") +
                " dev_bots_scrim=" + (DevBotsScrimEnabled() ? "1" : "0") +
                " debug=" + (DebugEnabled() ? "1" : "0") + " cfg_exec=" + (CfgExecEnabled() ? "1" : "0") +
                " scrim_auto=" + (f.autoEnabled ? "1" : "0") +
                " | events=" + (GameEventsListenerInstalled() ? "1" : "0") +
                " clientprint=" + (ClientPrintAvailable() ? "1" : "0"));
  return out;
}

}  // namespace readyup
