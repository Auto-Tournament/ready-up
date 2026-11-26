#include "readyup/ready_hud.h"

#include "readyup/plugin_api.h"

#include "readyup/admin_call.h"
#include "readyup/end_screen.h"
#include "readyup/engine.h"
#include "readyup/host.h"
#include "readyup/hud_screens.h"
#include "readyup/i18n_api.h"
#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/match_events.h"
#include "readyup/match_features.h"
#include "readyup/match_rules.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/player_registry.h"
#include "readyup/scrim_flow.h"
#include "readyup/players.h"
#include "readyup/server_settings.h"
#include "readyup/text.h"
#include "readyup/webhook.h"
#include "readyup/votes.h"
#include "readyup/walkover.h"
#include "readyup/welcome.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace readyup {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kMaxNamesPerSide = 6;
constexpr size_t kMaxNameBytes = 12;

struct Entry {
  uint64_t sid = 0;
  std::string name;
  int side = 0;  // 2 = T, 3 = CT
  bool ready = false;
  bool connected = true;
};

struct TeamLine {
  std::string name;
  TeamHudState st;
};

struct Board {
  bool teamView = false;  // rule team_ready: per-team lines (match warmup, not scrim, not bot fill)
  TeamLine teams[2];      // [0] = T (side 2), [1] = CT (side 3); set when teamView
  std::string titleKey;      // locales: hud.title_*
  std::string countdownKey;  // hud.countdown_knife / _live (empty when none)
  int countdownSecs = 0;
  std::vector<Entry> entries;
  int botsCt = 0;
  int botsT = 0;
  int ready = 0;
  int total = 0;
  bool tallied = false;  // ready/total set per team (match); else counted from entries (scrim)
  bool scrim = false;
  std::string noteKey;  // hud.note_* (empty when none)
};

Clock::time_point g_lastRun{};
std::atomic<bool> g_showing{false};

static std::string NameOf(uint64_t sid, const std::unordered_map<uint64_t, std::string>& names) {
  auto it = names.find(sid);
  if (it != names.end() && !it->second.empty()) return it->second;
  for (const auto& p : ListObservedPlayers()) {
    if (p.steamid64 == sid && !p.name.empty()) return p.name;
  }
  const std::string s = std::to_string(static_cast<unsigned long long>(sid));
  return "#" + s.substr(s.size() > 4 ? s.size() - 4 : 0);
}

static Board BuildBoard(ReadyUpMode mode, const std::optional<WebhookMatchContext>& ctx,
                        const std::vector<HumanIdentity>& humans) {
  Board b;
  std::unordered_map<uint64_t, std::string> names;
  for (const auto& h : humans) names[h.steamid64] = h.name;

  if (mode == ReadyUpMode::ScrimWarmup || !ctx) {
    b.scrim = true;
    b.titleKey = "hud.title_scrim_warmup";
    const auto roster = BuildScrimRoster();
    for (const auto& kv : roster.teamNum) {
      Entry e;
      e.sid = kv.first;
      e.side = kv.second;
      e.name = NameOf(kv.first, names);
      e.ready = IsReady(kv.first);
      b.entries.push_back(std::move(e));
    }
    for (const auto& kv : roster.devBots) {
      if (kv.second == 3) b.botsCt++;
      else if (kv.second == 2) b.botsT++;
    }
    if (!CountScrimRoster(roster).bothSides) {
      b.noteKey = DevBotsReadyEnabled() ? "hud.note_both_sides_bots" : "hud.note_both_sides";
    }
    const int cd = ScrimCountdownSecondsLeft();
    if (cd >= 0) {
      b.countdownKey = settings::Bool("knife_enabled_default") ? "hud.countdown_knife" : "hud.countdown_live";
      b.countdownSecs = cd;
    }
  } else {
    b.scrim = (ctx->slug == "scrim");
    b.titleKey = RecoveryGateEnabled() ? "hud.title_recovered" : (b.scrim ? "hud.title_scrim" : "hud.title_match_warmup");
    const auto ms = MatchStateGet();
    const int mapNum = ms.map_number <= 0 ? 1 : ms.map_number;
    bool team1Ct = true;
    if (mapNum >= 1 && static_cast<size_t>(mapNum) <= ctx->map_sides.size() &&
        ctx->map_sides[static_cast<size_t>(mapNum - 1)] == "team2_ct") {
      team1Ct = false;
    }
    std::unordered_set<uint64_t> connected;
    for (const auto& h : humans) connected.insert(h.steamid64);
    int teamRoster[2] = {0, 0}, teamReady[2] = {0, 0}, teamPresent[2] = {0, 0}, teamPresentReady[2] = {0, 0};
    for (const auto& kv : ctx->roster_team) {
      if (kv.first == 0) continue;
      Entry e;
      e.sid = kv.first;
      const bool t1 = (kv.second == WebhookTeam::Team1);
      e.side = (t1 == team1Ct) ? 3 : 2;
      e.name = NameOf(kv.first, names);
      e.ready = IsReady(kv.first);
      e.connected = connected.count(kv.first) != 0;
      ++teamRoster[t1 ? 0 : 1];
      if (e.ready) ++teamReady[t1 ? 0 : 1];
      if (e.connected) {
        ++teamPresent[t1 ? 0 : 1];
        if (e.ready) ++teamPresentReady[t1 ? 0 : 1];
      }
      // Bot fill: roster players who are not here are not waited for, so not listed.
      if (ctx->bot_fill && !e.connected) continue;
      b.entries.push_back(std::move(e));
    }
    // Substitutes never raise the total (match_rules.h TeamReadyTally): 5 + 1 sub reads x/5.
    // Bot fill counts the connected humans only (BotFillReadyTally): 2 humans + 3 bots reads x/2.
    for (int t = 0; t < 2; ++t) {
      const ReadyTally tally = ctx->bot_fill
                                   ? BotFillReadyTally(teamPresent[t], teamPresentReady[t])
                                   : TeamReadyTally(teamRoster[t], teamReady[t], ctx->players_per_team);
      b.ready += tally.ready;
      b.total += tally.total;
    }
    b.tallied = true;
    if (!b.scrim && !ctx->bot_fill && EffectiveRules().team_ready) {
      b.teamView = true;
      for (int t = 0; t < 2; ++t) {
        const int side = ((t == 0) == team1Ct) ? 3 : 2;
        TeamLine& tl = b.teams[side == 3 ? 1 : 0];
        tl.name = (t == 0) ? ctx->team1_name : ctx->team2_name;
        tl.st = TeamHudStatus(teamRoster[t], teamPresent[t], teamPresentReady[t], ctx->players_per_team);
      }
    }
    if (DevBotsReadyEnabled()) {
      for (const auto& bot : ListBots()) {
        if (bot.team == 3) b.botsCt++;
        else if (bot.team == 2) b.botsT++;
      }
    }
  }
  std::sort(b.entries.begin(), b.entries.end(),
            [](const Entry& x, const Entry& y) { return x.name < y.name; });
  if (!b.tallied) {
    for (const auto& e : b.entries) {
      b.total++;
      if (e.ready) b.ready++;
    }
  }
  return b;
}

// The viewer's text (locales: hud.*).
static std::string T(const std::string& lang, const char* key, const text::Args& args = {}) {
  return text::ForLang(lang, key, args);
}
static std::string N(int v) { return std::to_string(v); }
static std::string MinSec(int secs) {
  return std::to_string(secs / 60) + ":" + (secs % 60 < 10 ? "0" : "") + std::to_string(secs % 60);
}

// ---- the clickable HUD (hud_screens.h): one spec per player --------------------------------

static std::string L(const char* k, const std::string& v) { return std::string(k) + "=" + v + "\n"; }

// Warmup: the card (left, above the chat) with Ready / Unready; the countdown in the centre.
static std::string ReadySpec(const Board& b, uint64_t viewer, const std::string& name, bool welcome, const std::string& lang) {
  const int cd = b.scrim ? ScrimCountdownSecondsLeft() : -1;
  if (cd >= 0) {
    const bool knife = settings::Bool("knife_enabled_default");
    return L("id", "countdown") + L("color", "live") + L("kind", T(lang, "spec.everyone_ready")) +
           L("title", T(lang, knife ? "spec.knife_in" : "spec.live_in", {{"seconds", N(cd)}}));
  }
  const Entry* me = nullptr;
  int ready[2] = {0, 0}, total[2] = {0, 0};  // [0] CT, [1] T
  for (const auto& e : b.entries) {
    if (e.sid == viewer) me = &e;
    const int t = e.side == 3 ? 0 : 1;
    ++total[t];
    if (e.ready) ++ready[t];
  }
  ready[0] += b.botsCt;
  total[0] += b.botsCt;
  ready[1] += b.botsT;
  total[1] += b.botsT;
  // The centre panel: in the buy menu it sits in the empty dropped-weapons area under the grid
  // (Sivert, 2026-10-27; covering part of the menu is fine).
  // No brand row and tight padding: short enough to sit under the buy grid, in the dropped-weapons
  // area (the full card covered the grid's last row).
  std::string s = L("id", "warm") + L("cls", "no-brand pad-s");
  std::string title, sub, btn;
  const std::string readyBtn = "btn=" + T(lang, "spec.btn_ready") + "|green|pulse|.r\n";
  const std::string unreadyBtn = "btn=" + T(lang, "spec.btn_unready") + "|ghost||.ur\n";
  bool ok = false;
  if (b.teamView && me) {
    const TeamHudState& st = b.teams[me->side == 3 ? 1 : 0].st;
    ok = st.ready;
    title = T(lang, st.ready ? "spec.team_ready" : st.allHere ? "spec.team_not_ready" : "spec.team_waiting");
    if (!st.allHere) sub = T(lang, "spec.here", {{"here", N(st.here)}, {"total", N(st.total)}});
    if (st.ready) btn = unreadyBtn;
    else if (st.allHere) btn = readyBtn;
    for (int t = 0; t < 2; ++t) {
      const TeamHudState& ts = b.teams[t == 0 ? 1 : 0].st;
      total[t] = ts.total;
      ready[t] = ts.ready ? ts.total : 0;
    }
  } else if (me) {
    ok = me->ready;
    title = T(lang, me->ready ? "spec.you_ready" : "spec.you_not_ready");
    btn = me->ready ? unreadyBtn : readyBtn;
  } else {
    title = T(lang, b.scrim ? "spec.join_team" : "spec.spectating");
  }
  const std::string kind = welcome ? T(lang, "spec.welcome", {{"name", HudSpecText(name)}})
                                   : T(lang, ok ? "spec.kind_ready" : b.scrim ? "spec.kind_scrim" : "spec.kind_warmup");
  s += L("color", ok ? "live" : "acc") + L("kind", kind) + L("title", title);
  if (!sub.empty()) s += L("sub", HudSpecText(sub));
  const std::string ctName = b.teamView && !b.teams[1].name.empty() ? HudSpecText(b.teams[1].name) : "CT";
  const std::string tName = b.teamView && !b.teams[0].name.empty() ? HudSpecText(b.teams[0].name) : "T";
  if (total[0] > 0 || total[1] > 0) {
    s += "teams=" + ctName + "|" + std::to_string(std::min(ready[0], 5)) + "|" + std::to_string(std::clamp(total[0], 0, 5)) +
         "|" + tName + "|" + std::to_string(std::min(ready[1], 5)) + "|" + std::to_string(std::clamp(total[1], 0, 5)) + "\n";
  }
  const std::string count = std::to_string(b.ready) + " / " + std::to_string(b.total);
  // v4 (Hold E): no ready card any more. The chip above the AT logo says the button and the count
  // (hud.so where=chip), green once ready; holding E presses it. Spectators and players without a
  // button (waiting for the team) still get the small card below.
  if (!btn.empty()) {
    // One id for both states: the chip updates in place (slides down, turns green) instead of
    // fading out and in when you ready up (Sivert, 2026-11-24).
    return L("id", "readychip") + L("where", "chip") + L("color", ok ? "live" : "acc") + L("kind", kind) +
           L("title", title) + btn + L("counter", count) + L("hold", "1");
  }
  // Ready: the card shrinks to a one-line pill (dots, count, a small Unready), welcome or not. It
  // waited for the welcome to end, which starts over at every respawn, so in warmup it stayed big
  // (2026-11-07).
  if (ok && !btn.empty()) {
    s += L("id", "ready") + L("kind", T(lang, "spec.kind_ready") + " \xC2\xB7 " + count) + L("size", "mini") + btn + L("bw", "s") + L("mouse", "b") + L("hold", "1");
    return s;
  }
  // The team dots already say who is ready; the count only stands in when there are none (Miller).
  if (total[0] == 0 && total[1] == 0) s += L("counter", count);
  // Ready can be typed as well: the button needs the buy menu (B), which a dead player can't open.
  if (!btn.empty()) {
    s += btn + L("bw", "l") + L("hold", "1") + L("hint", T(lang, "spec.press_b")) +
         L("cmd", T(lang, me && me->ready ? "spec.hold_unready" : "spec.hold_ready") + (me && me->ready ? "|.ur" : "|.r")) +
         L("mouse", "b");
  }
  return s;
}

// The viewer's team is not full on the server (the full-team timer shows on their card). Without
// the team tally (scrims, bot fill) everyone sees it.
static bool ViewerTeamShort(const Board& b, uint64_t viewer) {
  if (!b.teamView) return true;
  for (const auto& e : b.entries) {
    if (e.sid == viewer) return !b.teams[e.side == 3 ? 1 : 0].st.allHere;
  }
  return false;
}

// Knife: "knives only" before the fight, then the side pick (winners) or the wait (everyone else).
static std::string KnifeSpec(const KnifeHudInfo& k, int viewerSide, uint64_t viewer, const std::string& lang) {
  if (k.phase == KnifePhase::Starting) {
    // Orange, not green: green read as "someone already won" (Sivert, 2026-11-24). Green = done / live.
    return L("id", "knife") + L("kind", T(lang, "spec.knife_round")) + L("title", T(lang, "spec.winners_pick"));
  }
  const std::string secs = std::to_string(std::max(0, k.secondsLeft));
  if (viewerSide == k.winnerCs && k.captain && viewer != k.captain) {
    // The captain picks (a captain is set): the rest of the team only sees who is picking.
    return L("id", "captain") + L("color", "live") + L("kind", T(lang, "spec.you_won_knife")) +
           L("title", T(lang, "spec.team_picking", {{"team", HudSpecText(k.captainName)}})) + L("timer", secs);
  }
  if (viewerSide == k.winnerCs) {
    // The menu panel on the left (v4, Hold E): W S choose, holding E picks. The side the team is on
    // now comes first, so holding E at once stays.
    const std::string rows = viewerSide == 2 ? "btn=T|t||.t\nbtn=CT|ct||.ct\n" : "btn=CT|ct||.ct\nbtn=T|t||.t\n";
    return L("id", "pick") + L("where", "left") + L("color", "live") + L("kind", T(lang, "spec.you_won_knife")) +
           L("title", T(lang, "spec.which_side")) + L("timer", secs) + rows + L("mouse", "on") +
           L("closerow", "0");  // a choice that must be made: no Close (Sivert, 2026-11-24)
  }
  const bool playing = viewerSide == 2 || viewerSide == 3;
  return L("id", "lost") + L("kind", T(lang, playing ? "spec.you_lost_knife" : "spec.knife_round")) +
         L("title", T(lang, "spec.team_picking", {{"team", HudSpecText(k.winnerName)}})) + L("timer", secs);
}

// Live map: pauses (with the unpause button) and a missing team.
static std::string LiveSpec(const LiveHudInfo& l, int viewerSide, const std::string& lang) {
  const WebhookTeam mine = VotesTeamForSide(viewerSide);
  const std::string t1 = HudSpecText(l.team1.empty() ? T(lang, "spec.team1") : l.team1);
  const std::string t2 = HudSpecText(l.team2.empty() ? T(lang, "spec.team2") : l.team2);
  const std::string paused = T(lang, "spec.paused");
  auto confirmAndButton = [&](bool onlyPausing) {
    std::string s = "confirm=" + t1 + "|" + (l.team1Confirmed ? "1" : "0") + "|" + t2 + "|" + (l.team2Confirmed ? "1" : "0") + "\n";
    if (mine == WebhookTeam::Unknown) return s;
    const std::string myName = mine == WebhookTeam::Team1 ? l.team1 : l.team2;
    if (onlyPausing && !l.byTeam.empty() && myName != l.byTeam) return s;
    const bool done = mine == WebhookTeam::Team1 ? l.team1Confirmed : l.team2Confirmed;
    if (done)
      return s + "btn=" + T(lang, "spec.waiting_for", {{"team", mine == WebhookTeam::Team1 ? t2 : t1}}) + "|ghost|off|\n" + L("bw", "xl");
    return s + "btn=" + T(lang, "spec.btn_unpause") + "|warn|pulse|.up\n" + L("bw", "l") + L("mouse", "on") + L("hold", "1");
  };
  if (l.paused && l.pendingFreeze) {
    // Asked for mid-round: the round goes on until freeze time. Only the pausing team is told, on
    // the side card, with no buttons and no cursor (the full card took the mouse mid-round,
    // 2026-10-27); everyone else sees the pause when it happens.
    const std::string myName = mine == WebhookTeam::Team1 ? l.team1 : mine == WebhookTeam::Team2 ? l.team2 : std::string();
    if (l.byTeam.empty() || myName != l.byTeam) return {};
    return L("id", "pausing") + L("where", "card") + L("color", "warn") +
           L("kind", T(lang, l.type == "tactical" ? "spec.timeout" : "spec.tech_pause")) + L("title", T(lang, "spec.starts_at_freeze"));
  }
  if (l.paused) {
    if (l.type == "technical") {
      std::string s = L("id", "pause") + L("color", "warn") + L("kind", T(lang, "spec.tech_pause")) +
                      L("title", l.byTeam.empty() ? paused : T(lang, "spec.team_paused", {{"team", HudSpecText(l.byTeam)}}));
      if (l.secondsLeft >= 0) s += L("sub", T(lang, "spec.auto_unpause_in", {{"seconds", N(l.secondsLeft)}}));
      return s + confirmAndButton(!l.bothRequired);
    }
    if (l.type == "tactical") {
      return L("id", "tac") + L("kind", T(lang, "spec.timeout")) +
             L("title", l.byTeam.empty() ? paused : T(lang, "spec.team_called_it", {{"team", HudSpecText(l.byTeam)}}));
    }
    if (l.type == "halftime" || l.type == "auto_5v5" || l.type == "restore" || l.type == "engine") {
      const char* kind = l.type == "halftime" ? "spec.halftime"
                         : l.type == "restore" ? "spec.round_restored"
                         : l.type == "engine"  ? "spec.game_paused"
                                               : "spec.not_5v5";
      std::string s = L("id", "pause_" + l.type) + L("color", l.type == "auto_5v5" ? "ban" : "warn") + L("kind", T(lang, kind)) +
                      L("title", l.type == "auto_5v5" && !l.byTeam.empty() ? T(lang, "spec.team_short", {{"team", HudSpecText(l.byTeam)}}) : paused);
      return s + confirmAndButton(false);
    }
    return L("id", "adminpause") + L("color", "warn") + L("kind", T(lang, "spec.admin_pause")) + L("title", T(lang, "spec.paused_by_admin"));
  }
  if (l.forfeit) {
    return L("id", "missing") + L("color", "ban") + L("kind", T(lang, "spec.forfeit")) +
           L("title", T(lang, "spec.team_missing", {{"team", HudSpecText(l.forfeitTeam)}})) +
           L("sub", T(lang, "spec.forfeit_in", {{"seconds", N(std::max(0, l.forfeitSecondsLeft))}}));
  }
  return {};
}

static bool HudEnabled() {
  return Cfg().ready_hud;
}

// Last per-client send result; assume it works until a send fails.
std::atomic<bool> g_sendOk{true};

}  // namespace

// The HUD shows ready-up when hud.so draws its screens; else chat does (ChatUnlessHud).
bool HudReplacesChat() { return HudEnabled() && HudScreensOn(); }

bool ReadyHudShowing() {
  return g_showing.load(std::memory_order_relaxed) && HudEnabled();
}

void ReadyHudTick() {
  if (IsDisabled()) return;
  const auto now = Clock::now();
  if (g_lastRun.time_since_epoch().count() != 0 && (now - g_lastRun) < std::chrono::milliseconds(Cfg().hud_tick_ms)) return;
  g_lastRun = now;

  const bool enabled = HudEnabled();
  if (!enabled) {
    g_showing.store(false, std::memory_order_relaxed);
    return;
  }

  const ReadyUpMode mode = GetMode();
  const auto ctx = WebhookGetMatchContext();

  enum class What { None, Ready, Knife, Live };
  What what = What::None;
  KnifeHudInfo knife;
  LiveHudInfo live;
  if (!enabled) {
    what = What::None;
  } else if (mode == ReadyUpMode::ScrimWarmup) {
    what = What::Ready;
  } else if (mode == ReadyUpMode::MatchWarmup && ctx && WarmupEnabled() && !GoLiveTriggered()) {
    what = What::Ready;
  } else if (mode == ReadyUpMode::MatchKnife) {
    knife = KnifeHudSnapshot();
    // knife.cfg has almost no freeze time, so "Starting" lasts about a second.
    // Keep the KNIFE ROUND panel up for hud_knife_hold_s after the knife round
    // begins, also while it is running.
    static Clock::time_point s_knifeSince{};
    if (knife.phase == KnifePhase::None) {
      s_knifeSince = {};
    } else if (s_knifeSince == Clock::time_point{}) {
      s_knifeSince = Clock::now();
    }
    const bool holding = knife.phase == KnifePhase::Running && s_knifeSince != Clock::time_point{} &&
                         (Clock::now() - s_knifeSince) < std::chrono::seconds(Cfg().hud_knife_hold_s);
    // The pick card waits for the round after the knife round (KnifeHudInfo::pickOpen).
    if (knife.phase == KnifePhase::Starting || (knife.phase == KnifePhase::Picking && knife.pickOpen) || holding) {
      what = What::Knife;
      if (holding) knife.phase = KnifePhase::Starting;  // same "knives only" text while held
    }
  } else if (mode == ReadyUpMode::MatchLive) {
    live = MatchFeaturesHud();
    if (live.paused || live.forfeit) what = What::Live;  // a pause or a missing team
  }

  // The clickable HUD draws these screens (and menus and votes on top) when it can; the center
  // HTML then only carries them for players who turned the HUD off.
  if (enabled && HudScreensOn()) {
    const Board hudBoard = what == What::Ready ? BuildBoard(mode, ctx, ListHumans()) : Board{};
    HudScreensRun([&](const HumanIdentity& h) -> std::string {
      const int slot = h.slot >= 0 ? h.slot : h.userid;
      const std::string lang = i18n::LanguageOf(host::Api(), slot);
      if (std::string e = EndScreenHudSpec(slot, lang); !e.empty()) return e;
      // The walkover choice can't be skipped: it replaces the ready card (walkover.h).
      if (ctx && mode == ReadyUpMode::MatchWarmup) {
        if (std::string w = WalkoverHudSpec(*ctx, h.steamid64, lang); !w.empty()) return w;
      }
      if (what == What::Ready) {
        std::string s = ReadySpec(hudBoard, h.steamid64, h.name, WelcomeActiveFor(slot, h.steamid64), lang);
        // The platform's full-team timer: the countdown on the card while the viewer's team isn't full.
        const long long left = ctx ? FullTeamSecondsLeft(*ctx) : -1;
        if (left >= 0 && ViewerTeamShort(hudBoard, h.steamid64)) s += L("timer", std::to_string(left));
        return s;
      }
      if (what == What::Knife) return KnifeSpec(knife, h.team, h.steamid64, lang);
      if (mode == ReadyUpMode::MatchLive) return LiveSpec(live, h.team, lang);
      return {};
    });
    g_showing.store(true, std::memory_order_relaxed);
    // No centre panel next to the HUD: players who turned it off (.hud off) get chat lines instead
    // (ChatUnlessHud; Sivert, 2026-11-24: the old HTML panel should be gone).
  } else {
    // Without the clickable HUD (no hud.so, or hud_screens=0) there is no panel: the chat lines
    // carry ready-up (ChatUnlessHud). The old centre HTML panel is gone (Sivert, 2026-11-25).
    g_showing.store(false, std::memory_order_relaxed);
  }
}

}  // namespace readyup
