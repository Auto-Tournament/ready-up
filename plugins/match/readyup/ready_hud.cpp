#include "readyup/ready_hud.h"

#include "readyup/plugin_api.h"

#include "readyup/admin_call.h"
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

// Palette (matches the welcome screen).
constexpr const char* kCt = "#5EA8FF";
constexpr const char* kT = "#FF9D3B";
constexpr const char* kGold = "#FFD23F";
constexpr const char* kGrey = "#8A8F98";
constexpr const char* kDim = "#5B6068";
constexpr const char* kOk = "#7CD35A";
constexpr const char* kNo = "#F87171";

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

struct Sent {
  std::string html;
  Clock::time_point at{};
};

std::unordered_map<int, Sent> g_sent;  // GameFrame thread only
Clock::time_point g_lastRun{};
std::atomic<bool> g_showing{false};

// `.ru hud test <n>` requests: steamid64 -> (variant, shown until).
constexpr auto kTestShowFor = std::chrono::seconds(10);
std::mutex g_testMu;
std::unordered_map<uint64_t, std::pair<int, Clock::time_point>> g_tests;

// Public test images for `.ru hud test` (all checked to return 200 with curl).
constexpr const char* kTestSvg = "https://raw.githubusercontent.com/Auto-Tournament/auto-tournament/main/client/public/icon.svg";
constexpr const char* kTestPng = "https://raw.githubusercontent.com/Auto-Tournament/auto-tournament/main/client/public/icon-192.png";
constexpr const char* kTestPngWiki =
    "https://upload.wikimedia.org/wikipedia/commons/thumb/4/47/PNG_transparency_demonstration_1.png/"
    "250px-PNG_transparency_demonstration_1.png";

static std::string Esc(const std::string& in, size_t maxBytes) {
  std::string s = in;
  if (s.size() > maxBytes) {
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
    s += "..";
  }
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
        break;
    }
  }
  return out;
}

static std::string Font(const char* color, const std::string& inner) {
  return std::string("<font color='") + color + "'>" + inner + "</font>";
}

// A URL that can sit inside a single-quoted HTML attribute (http/https only).
static std::string AttrSafeUrl(const std::string& url) {
  if (url.empty() || url.size() > 300) return {};
  if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return {};
  for (unsigned char c : url) {
    if (c <= 0x20 || c == '\'' || c == '"' || c == '<' || c == '>' || c == '\\' || c >= 0x7F) return {};
  }
  return url;
}

static std::string Img(const std::string& url, int height) {
  return "<img src='" + url + "' width='" + std::to_string(height) + "' height='" + std::to_string(height) + "'>";
}

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

// One side's line. Entries are grouped by color (ready, not ready, not
// connected) so each run costs one <font> tag: keeps 5v5 well under ~1 KB.
static std::string SideLine(const Board& b, int side, uint64_t viewer, const std::string& lang) {
  if (b.teamView) {
    const TeamLine& t = b.teams[side == 3 ? 1 : 0];
    std::string s = Font(side == 3 ? kCt : kT, side == 3 ? "<b>CT</b>" : "<b>T</b>");
    if (!t.name.empty()) s += " " + Esc(t.name, 16);
    if (t.st.ready) return s + " " + Font(kOk, "<b>&#10004; " + T(lang, "hud.side_ready") + "</b>");
    return s + " " + Font(kNo, "&#10006; " + T(lang, "hud.side_not_ready")) +
           Font(kDim, " &#183; " + T(lang, "hud.side_here", {{"here", N(t.st.here)}, {"total", N(t.st.total)}}));
  }
  std::string s = Font(side == 3 ? kCt : kT, side == 3 ? "<b>CT</b>" : "<b>T</b>");
  std::vector<const Entry*> list;
  for (const auto& e : b.entries) {
    if (e.side == side) list.push_back(&e);
  }
  auto rank = [](const Entry* e) { return !e->connected ? 2 : (e->ready ? 0 : 1); };
  std::stable_sort(list.begin(), list.end(), [&](const Entry* x, const Entry* y) { return rank(x) < rank(y); });

  int shown = 0, hidden = 0;
  const char* runColor = nullptr;
  std::string run;
  auto flush = [&]() {
    if (runColor && !run.empty()) s += "&#160; " + Font(runColor, run);
    run.clear();
  };
  for (const Entry* e : list) {
    // Keep the viewer visible even past the cap.
    if (shown >= kMaxNamesPerSide && e->sid != viewer) {
      hidden++;
      continue;
    }
    const char* col = !e->connected ? kDim : (e->ready ? kOk : kNo);
    if (col != runColor) {
      flush();
      runColor = col;
    } else {
      run += "&#160; ";
    }
    run += e->ready ? "&#10004;" : "&#10006;";
    const std::string nm = Esc(e->name, kMaxNameBytes);
    run += (e->sid == viewer) ? "<b>" + nm + "</b>" : nm;
    shown++;
  }
  flush();
  if (hidden > 0) s += " " + Font(kGrey, "+" + std::to_string(hidden));
  const int bots = (side == 3) ? b.botsCt : b.botsT;
  if (bots > 0) s += " " + Font(kOk, "&#10004;" + T(lang, "hud.bots", {{"count", N(bots)}}));
  if (shown == 0 && hidden == 0 && bots == 0) s += " " + Font(kDim, "-");
  return s;
}

static std::string ReadyHtml(const Board& b, uint64_t viewer, const std::string& footer, const std::string& lang) {
  const Entry* me = nullptr;
  for (const auto& e : b.entries) {
    if (e.sid == viewer) me = &e;
  }
  std::string h;
  h.reserve(1024);
  // The viewer's own state first and big: players missed the small line under the
  // names and sat in warmup not knowing they were the one holding it up.
  if (b.teamView && me) {
    const TeamHudState& st = b.teams[me->side == 3 ? 1 : 0].st;
    if (st.ready) {
      h += "<font class='fontSize-l' color='" + std::string(kOk) + "'><b>" + T(lang, "hud.team_ready") + "</b></font><br>";
    } else if (st.allHere) {
      h += "<font class='fontSize-xl' color='" + std::string(kNo) + "'><b>" + T(lang, "hud.team_not_ready") + "</b></font><br>";
      h += Font(kGold, T(lang, "hud.type_r_team")) + "<br>";
    } else {
      h += "<font class='fontSize-xl' color='" + std::string(kNo) + "'><b>" +
           T(lang, "hud.team_waiting", {{"here", N(st.here)}, {"total", N(st.total)}}) + "</b></font><br>";
      h += Font(kGold, T(lang, "hud.one_r_team", {{"count", N(st.total)}})) + "<br>";
    }
  } else if (me && !me->ready) {
    h += "<font class='fontSize-xl' color='" + std::string(kNo) + "'><b>" + T(lang, "hud.you_not_ready") + "</b></font><br>";
    h += Font(kGold, T(lang, "hud.type_r")) + "<br>";
  } else if (me) {
    h += "<font class='fontSize-l' color='" + std::string(kOk) + "'><b>" + T(lang, "hud.you_ready") + "</b></font><br>";
  }
  const std::string title = T(lang, b.titleKey.c_str());
  const std::string brand = HudBrandHtml(/*imgHeight=*/20, "");
  if (!brand.empty()) h += brand + " " + Font(kGrey, "&#183; " + title);
  else h += Font(kGrey, title);
  if (!b.countdownKey.empty())
    h += " " + Font(kGold, "&#183; <b>" + T(lang, b.countdownKey.c_str(), {{"seconds", N(b.countdownSecs)}}) + "</b>");
  h += "<br>";
  h += Font(b.total > 0 && b.ready == b.total ? kOk : kGold,
            "<b>" + T(lang, "hud.ready_count", {{"ready", N(b.ready)}, {"total", N(b.total)}}) + "</b>");
  h += "<br>" + SideLine(b, 3, viewer, lang) + "<br>" + SideLine(b, 2, viewer, lang) + "<br>";

  if (!me) {
    h += Font(kGrey, T(lang, b.scrim ? "hud.join_team" : "hud.spectating"));
  } else if (me->ready) {
    h += Font(kGrey, T(lang, "hud.ur_cancel"));
  }
  if (!b.noteKey.empty()) h += "<br>" + Font(kGrey, T(lang, b.noteKey.c_str()));
  if (!footer.empty()) h += "<br>" + footer;
  return h;
}

// modes.cpp's knifeReasonShort -> locales key.
static const char* KnifeReasonKey(const std::string& r) {
  return r == "elimination" ? "hud.reason_elimination"
         : r == "more alive" ? "hud.reason_more_alive"
         : r == "more HP"    ? "hud.reason_more_hp"
                             : "hud.reason_coin_flip";
}

static std::string KnifeHtml(const KnifeHudInfo& k, int viewerSide, const std::string& lang) {
  std::string h;
  h.reserve(384);
  h += Font(kGold, "<b>" + T(lang, "hud.knife_title") + "</b>") + "<br>";
  if (k.phase == KnifePhase::Starting) {
    h += T(lang, "hud.knife_starting");
    return h;
  }
  const char* col = (k.winnerCs == 3) ? kCt : kT;
  h += T(lang, "hud.knife_won", {{"team", Font(col, "<b>" + Esc(k.winnerName, 24) + "</b>")}});
  if (!k.reasonShort.empty()) h += " " + Font(kGrey, "(" + T(lang, KnifeReasonKey(k.reasonShort)) + ")");
  h += "<br>";
  const std::string secs = N(k.secondsLeft);
  if (viewerSide == k.winnerCs) {
    h += Font(kOk, T(lang, "hud.knife_type_pick")) + "<br>";
    h += Font(kGrey, T(lang, "hud.knife_no_pick", {{"seconds", secs}}));
  } else {
    h += Font(kGrey, T(lang, "hud.knife_waiting", {{"team", Esc(k.winnerName, 24)}, {"seconds", secs}}));
  }
  return h;
}

// Live map: pause state + countdowns, and the team-left forfeit countdown. No images (resent
// every frame, docs/HUD.md). Empty when there is nothing to show.
static std::string LiveHtml(const LiveHudInfo& l, const std::string& lang) {
  std::string h;
  auto mark = [&](bool ok, const std::string& n) { return Font(ok ? kOk : kDim, (ok ? "&#10004; " : "&#10006; ") + Esc(n, 16)); };
  if (l.paused) {
    if (l.type == "tactical") {
      h += Font(kGold, "<b>" + T(lang, "hud.tactical_timeout") + "</b>");
      if (!l.byTeam.empty()) h += "<br>" + Esc(l.byTeam, 24);
    } else if (l.type == "technical") {
      h += Font(kNo, "<b>" + T(lang, "hud.technical_pause") + "</b>");
      if (!l.byTeam.empty()) h += " " + Font(kGrey, T(lang, "hud.by_team", {{"team", Esc(l.byTeam, 24)}}));
      if (l.pendingFreeze) {
        h += "<br>" + Font(kGrey, T(lang, "hud.pausing_at_freeze"));
      } else if (l.secondsLeft >= 0) {
        h += "<br>" + T(lang, "hud.auto_unpause_in", {{"time", MinSec(l.secondsLeft)}});
      }
      h += "<br>" + Font(kGrey, T(lang, l.bothRequired ? "hud.unpause_both" : "hud.unpause_pausing")) + "<br>" +
           mark(l.team1Confirmed, l.team1) + " &#183; " + mark(l.team2Confirmed, l.team2);
    } else if (l.type == "halftime" || l.type == "auto_5v5" || l.type == "restore" ||
               l.type == "engine") {
      // Pauses that belong to nobody (an engine pause, or a round restore): both teams resume
      // them (match_features.h).
      if (l.type == "halftime") {
        h += Font(kGold, "<b>" + T(lang, "hud.halftime_pause") + "</b>");
      } else if (l.type == "restore") {
        h += Font(kGold, "<b>" + T(lang, "hud.round_restored") + "</b>");
      } else if (l.type == "engine") {
        h += Font(kNo, "<b>" + T(lang, "hud.game_paused") + "</b>");
      } else {
        h += Font(kNo, "<b>" + T(lang, "hud.not_5v5_paused") + "</b>");
        if (!l.byTeam.empty()) h += "<br>" + Font(kGrey, T(lang, "hud.team_short", {{"team", Esc(l.byTeam, 24)}}));
      }
      h += "<br>" + Font(kGrey, T(lang, "hud.unpause_both")) + "<br>" + mark(l.team1Confirmed, l.team1) +
           " &#183; " + mark(l.team2Confirmed, l.team2);
    } else {
      h += Font(kNo, "<b>" + T(lang, "hud.paused_by_admin") + "</b>") + "<br>" + Font(kGrey, T(lang, "hud.admin_unpauses"));
    }
  }
  if (l.forfeit) {
    if (!h.empty()) h += "<br>";
    h += Font(kNo, "<b>" + T(lang, "hud.team_left", {{"team", Esc(l.forfeitTeam, 24)}}) + "</b>") + "<br>" +
         T(lang, "hud.forfeit_in", {{"time", MinSec(l.forfeitSecondsLeft)}});
  }
  return h;
}

// ---- the clickable HUD (hud_screens.h): one spec per player, same data as the HTML above ------

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
  if (ok && !welcome && !btn.empty()) {
    // Ready: the card shrinks to a one-line pill (dots, count, a small Unready).
    s += L("id", "ready") + L("kind", T(lang, "spec.kind_ready") + " \xC2\xB7 " + count) + L("size", "mini") + btn + L("bw", "s") + L("mouse", "b");
    return s;
  }
  // The team dots already say who is ready; the count only stands in when there are none (Miller).
  if (total[0] == 0 && total[1] == 0) s += L("counter", count);
  // Ready can be typed as well: the button needs the buy menu (B), which a dead player can't open.
  if (!btn.empty()) {
    s += btn + L("bw", "l") + L("hint", T(lang, "spec.press_b")) +
         L("cmd", T(lang, "spec.or_type") + (me && me->ready ? "|.ur" : "|.r")) + L("mouse", "b");
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
static std::string KnifeSpec(const KnifeHudInfo& k, int viewerSide, const std::string& lang) {
  if (k.phase == KnifePhase::Starting) {
    return L("id", "knife") + L("color", "live") + L("kind", T(lang, "spec.knife_round")) + L("title", T(lang, "spec.winners_pick"));
  }
  const std::string secs = std::to_string(std::max(0, k.secondsLeft));
  if (viewerSide == k.winnerCs) {
    return L("id", "pick") + L("color", "live") + L("kind", T(lang, "spec.you_won_knife")) + L("title", T(lang, "spec.which_side")) +
           L("timer", secs) + "btn=CT|ct||.ct\nbtn=T|t||.t\n" + L("mouse", "on");
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
    return s + "btn=" + T(lang, "spec.btn_unpause") + "|warn|pulse|.up\n" + L("bw", "l") + L("mouse", "on");
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

// `.ru hud test <n>` variants. Each one fits well under ~1 KB.
static std::string TestHtml(int n) {
  const std::string title = Font(kGrey, "hudtest " + std::to_string(n)) + "<br>";
  switch (n) {
    case 1:
      return title +
             "<font class='fontSize-s'>fontSize-s</font> <font class='fontSize-sm'>sm</font> "
             "<font class='fontSize-m'>m</font><br>"
             "<font class='fontSize-l'>fontSize-l</font> <font class='fontSize-xl'>xl</font> "
             "<font class='fontSize-xxl'>xxl</font><br>"
             "<span class='fontWeight-Bold'>fontWeight-Bold</span> <b>b-tag</b> <i>i-tag</i> <u>u-tag</u><br>" +
             Font(kGold, "gold") + " " + Font(kCt, "CT") + " " + Font(kT, "T") + " " + Font(kOk, "ok") + " " +
             Font(kNo, "no") + " <span style='color:#C084FC'>span-style</span> <font color='red'>named-red</font>";
    case 2:
      return title + "A svg " + Img(kTestSvg, 32) + " | B png " + Img(kTestPng, 32) + "<br>C wiki png " +
             Img(kTestPngWiki, 40) + "<br>" + Font(kGrey, "missing = that format does not render");
    case 3:
      return title +
             "raw utf-8: \xE2\x9C\x94 \xE2\x9C\x96 \xE2\x98\x85 \xC2\xB7 \xE2\x80\xA2 \xE2\x86\x92<br>"
             "decimal: &#10004; &#10006; &#9733; &#183; &#8226; &#8594;<br>"
             "hex: &#x2714; &#x2716; &#x2605;<br>"
             "named: &check; &cross; &star; &middot; &bull; &rarr; &amp; &lt;b&gt;<br>"
             "emoji: \xF0\x9F\x8F\x86 \xF0\x9F\x94\xAA \xE2\x9C\x85 \xE2\x9D\x8C";
    case 4:
      return title + "svg (Auto Tournament icon.svg, height 64)<br>" + Img(kTestSvg, 64);
    case 5:
      return title + "png (Auto Tournament icon-192.png, height 64)<br>" + Img(kTestPng, 64);
    case 6:
      return title + "png (wikimedia 250px, height 64)<br>" + Img(kTestPngWiki, 64);
    case 7: {
      const auto c = Cfg();
      const std::string url = AttrSafeUrl(c.hud_logo_url);
      std::string h = title + HudBrandHtml(24, "fontSize-l") + "<br>";
      h += Font(kGrey, "hud_logo_url=" + (url.empty() ? std::string("(none)") : Esc(url, 80)));
      if (!c.hud_logo_url.empty() && url.empty()) h += "<br>" + Font(kNo, "hud_logo_url rejected (needs http(s), no quotes/spaces)");
      return h;
    }
    // 8..11: measuring the center panel (how many lines, how wide, how images scale), so HUDs can
    // be designed to fit. Screenshot each and read the cut-off.
    case 8: {
      std::string h = title;
      for (int i = 1; i <= 18; ++i) h += std::string(i < 10 ? "row 0" : "row ") + std::to_string(i) + (i < 18 ? "<br>" : "");
      return h;
    }
    case 9:
      return title + "0123456789012345678901234567890123456789012345678901234567890123456789<br>" +
             "<font class='fontSize-s'>0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890</font><br>" +
             "<font class='fontSize-l'>01234567890123456789012345678901234567890123456789</font><br>" +
             "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW";
    case 10:
      return title + "a " + Img(kTestPng, 24) + " b " + Img(kTestPng, 48) + " c <img src='" + std::string(kTestPng) +
             "' style='width:32px;height:32px'> d <img src='" + std::string(kTestPng) + "' height='32'>";
    case 11:
      return title + "wide " + "<img src='" + std::string(kTestPngWiki) + "' width='200' height='40'><br>" +
             "e <img src='" + std::string(kTestPngWiki) + "' width='16' height='16'> f " + Img(kTestSvg, 16) + " g " +
             Img(kTestSvg, 96);
    default:
      return {};
  }
}

static const char* TestDescription(int n) {
  switch (n) {
    case 1: return "font classes (fontSize-s..xxl, fontWeight-Bold), b/i/u, colors";
    case 2: return "images: A = Auto Tournament SVG, B = Auto Tournament PNG, C = wikimedia PNG";
    case 3: return "unicode: raw UTF-8, decimal/hex/named entities, emoji";
    case 4: return "SVG only (Auto Tournament icon.svg)";
    case 5: return "PNG only (Auto Tournament icon-192.png)";
    case 6: return "PNG only (wikimedia 250px)";
    case 7: return "configured header (hud_logo_url + hud_brand)";
    case 8: return "height ruler: rows 01-18 (the last row you see = how many lines fit)";
    case 9: return "width ruler: digits in default / s / l fonts and W's (the last digit you see = the width)";
    case 10: return "PNG sizes: a width/height 24, b 48, c style 32px, d height 32 only";
    case 11: return "wide PNG 200x40, tiny PNG 16, SVG 16 and 96";
    default: return nullptr;
  }
}

static bool HudEnabled() {
  return Cfg().ready_hud;
}

// Last per-client send result; assume it works until a send fails.
std::atomic<bool> g_sendOk{true};

}  // namespace

bool HudReplacesChat() { return HudEnabled() && g_sendOk.load(std::memory_order_relaxed); }

std::string HudBrandHtml(int imgHeight, const char* fontClass) {
  if (!FeatureEnabled(Feature::HudBrand)) return {};
  const auto c = Cfg();
  std::string h;
  const std::string url = AttrSafeUrl(c.hud_logo_url);
  if (!url.empty()) h += Img(url, std::max(8, std::min(128, imgHeight)));
  if (!c.hud_brand.empty()) {
    if (!h.empty()) h += " ";
    h += "<font ";
    if (fontClass && *fontClass) h += std::string("class='") + fontClass + "' ";
    h += std::string("color='") + kGold + "'><b>" + Esc(c.hud_brand, 40) + "</b></font>";
  }
  return h;
}

// ---- `.ru hud anim`: how fast does the client redraw the panel? --------------------------------

namespace {

struct Anim {
  int hz = 64;
  Clock::time_point start{}, until{}, next{};
  int frame = 0;
};
std::mutex g_animMu;
std::unordered_map<uint64_t, Anim> g_anims;

std::string Blocks(int n) {
  std::string o;
  for (int i = 0; i < n; ++i) o += "\u2588";  // full block
  return o;
}

// Ease-out cubic over a 2 s loop: fast at the start, settling at the end.
double EaseOut(double p) { return 1.0 - (1.0 - p) * (1.0 - p) * (1.0 - p); }

std::string AnimHtml(const Anim& a, Clock::time_point now) {
  const double t = std::chrono::duration<double>(now - a.start).count();
  const double loop = 2.0;
  const double p = std::fmod(t, loop) / loop;
  const double v = EaseOut(p);
  constexpr int kSegments = 40;
  const int filled = static_cast<int>(v * kSegments + 0.5);
  std::string h = "<font class='fontSize-l' color='#FFFFFF'>anim " + std::to_string(a.hz) + " Hz &#183; frame " +
                  std::to_string(a.frame) + "</font><br>";
  if (filled > 0) h += "<font color='#4ADE80'>" + Blocks(filled) + "</font>";
  if (filled < kSegments) h += "<font color='#3F3F46'>" + Blocks(kSegments - filled) + "</font>";
  char stats[64];
  std::snprintf(stats, sizeof stats, "%5.1f%% &#183; t=%.2f s", v * 100.0, t);
  h += "<br><font class='fontSize-m' color='#A3E635'>" + std::string(stats) + "</font>";
  return h;
}

}  // namespace

std::string ReadyHudRequestAnim(uint64_t steamid64, int hz, int seconds) {
  if (steamid64 == 0) return {};
  hz = std::clamp(hz > 0 ? hz : 64, 1, 64);
  seconds = std::clamp(seconds > 0 ? seconds : 10, 1, 30);
  const auto now = Clock::now();
  Anim a;
  a.hz = hz;
  a.start = now;
  a.next = now;
  a.until = now + std::chrono::seconds(seconds);
  std::lock_guard<std::mutex> lk(g_animMu);
  g_anims[steamid64] = a;
  return std::to_string(hz) + " Hz for " + std::to_string(seconds) + " s";
}

bool ReadyHudAnimActive(uint64_t steamid64) {
  std::lock_guard<std::mutex> lk(g_animMu);
  return g_anims.count(steamid64) != 0;
}

void ReadyHudAnimTick() {
  const auto now = Clock::now();
  struct Send {
    int slot;
    std::string html;
  };
  std::vector<Send> sends;
  {
    std::lock_guard<std::mutex> lk(g_animMu);
    if (g_anims.empty()) return;
    for (auto it = g_anims.begin(); it != g_anims.end();) {
      if (now >= it->second.until) {
        it = g_anims.erase(it);
        continue;
      }
      Anim& a = it->second;
      if (now >= a.next) {
        const int slot = GameEventsSlotForSteam(it->first).value_or(-1);
        if (slot >= 0) {
          ++a.frame;
          sends.push_back(Send{slot, AnimHtml(a, now)});
        }
        // Next frame on the hz grid (no drift); skip missed frames rather than bursting.
        const auto step = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / a.hz));
        do a.next += step;
        while (a.next <= now);
      }
      ++it;
    }
  }
  for (const auto& s : sends) SendCenterHtml(s.slot, s.html, 1, RU_HTML_PRIO_NOTICE);
}

bool ReadyHudShowing() {
  return g_showing.load(std::memory_order_relaxed) && HudEnabled();
}

std::string ReadyHudRequestTest(uint64_t steamid64, int variant) {
  const char* desc = TestDescription(variant);
  if (!desc || steamid64 == 0) return {};
  std::lock_guard<std::mutex> lk(g_testMu);
  g_tests[steamid64] = {variant, Clock::now() + kTestShowFor};
  return desc;
}

void ReadyHudTick() {
  if (IsDisabled()) return;
  const auto now = Clock::now();
  if (g_lastRun.time_since_epoch().count() != 0 && (now - g_lastRun) < std::chrono::milliseconds(Cfg().hud_tick_ms)) return;
  g_lastRun = now;

  // Pending `.ru hud test` panels (shown even with ready_hud=0 and outside warmup).
  std::unordered_map<uint64_t, int> tests;
  {
    std::lock_guard<std::mutex> lk(g_testMu);
    for (auto it = g_tests.begin(); it != g_tests.end();) {
      if (now >= it->second.second) {
        it = g_tests.erase(it);
      } else {
        tests[it->first] = it->second.first;
        ++it;
      }
    }
  }

  const bool enabled = HudEnabled();
  if (!enabled) g_showing.store(false, std::memory_order_relaxed);
  if (!enabled && tests.empty()) {
    g_sent.clear();
    return;
  }

  const ReadyUpMode mode = GetMode();
  const auto ctx = WebhookGetMatchContext();

  enum class What { None, Ready, Knife, Live };
  What what = What::None;
  KnifeHudInfo knife;
  bool liveShown = false;
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
    if (knife.phase == KnifePhase::Starting || knife.phase == KnifePhase::Picking || holding) {
      what = What::Knife;
      if (holding) knife.phase = KnifePhase::Starting;  // same "knives only" text while held
    }
  } else if (mode == ReadyUpMode::MatchLive) {
    live = MatchFeaturesHud();
    liveShown = !LiveHtml(live, "en").empty();  // the same for every language
    if (liveShown) what = What::Live;
  }

  // The clickable HUD draws these screens (and menus and votes on top) when it can; the center
  // HTML then only carries `.ru hud test` panels.
  if (enabled && HudScreensOn()) {
    const Board hudBoard = what == What::Ready ? BuildBoard(mode, ctx, ListHumans()) : Board{};
    HudScreensRun([&](const HumanIdentity& h) -> std::string {
      const int slot = h.slot >= 0 ? h.slot : h.userid;
      const std::string lang = i18n::LanguageOf(host::Api(), slot);
      // The walkover choice can't be skipped: it replaces the ready card (walkover.h).
      if (ctx && mode == ReadyUpMode::MatchWarmup) {
        if (std::string w = WalkoverHudSpec(*ctx, h.steamid64); !w.empty()) return w;
      }
      if (what == What::Ready) {
        std::string s = ReadySpec(hudBoard, h.steamid64, h.name, WelcomeActiveFor(slot, h.steamid64), lang);
        // The platform's full-team timer: the countdown on the card while the viewer's team isn't full.
        const long long left = ctx ? FullTeamSecondsLeft(*ctx) : -1;
        if (left >= 0 && ViewerTeamShort(hudBoard, h.steamid64)) s += L("timer", std::to_string(left));
        return s;
      }
      if (what == What::Knife) return KnifeSpec(knife, h.team, lang);
      if (mode == ReadyUpMode::MatchLive) return LiveSpec(live, h.team, lang);
      return {};
    });
    g_showing.store(true, std::memory_order_relaxed);
    what = What::None;
  }
  if (what == What::None && tests.empty()) {
    g_sent.clear();
    return;
  }

  const auto humans = ListHumans();
  Board board;
  std::string footer;
  if (what == What::Ready) {
    board = BuildBoard(mode, ctx, humans);
    footer = WarmupHtmlCustom();
    if (footer.size() > 256) footer.clear();  // keep the panel small
  }

  std::unordered_set<int> present;
  static std::atomic<bool> s_warned{false};
  static std::atomic<bool> s_loggedSize{false};
  for (const auto& h : humans) {
    // Engine slot from events when known; else the log `<N>` (the player slot in CS2).
    const int slot = h.slot >= 0 ? h.slot : h.userid;
    if (h.steamid64 == 0 || slot < 0 || slot > 63) continue;
    present.insert(slot);

    std::string html;
    const auto t = tests.find(h.steamid64);
    if (t != tests.end()) {
      html = TestHtml(t->second);  // over the HUD and the welcome card
    } else {
      if (what == What::None) continue;
      // The welcome screen owns the panel while it is up (~5s after joining).
      if (WelcomeActiveFor(slot, h.steamid64)) continue;
      if (ReadyHudAnimActive(h.steamid64)) continue;  // `.ru hud anim` owns this player's panel
      if (AdminCallCardActiveFor(slot)) continue;      // an `.admin` call card (admin_call.h)
      const std::string lang = i18n::LanguageOf(host::Api(), slot);
      html = (what == What::Ready)   ? ReadyHtml(board, h.steamid64, footer, lang)
             : (what == What::Live) ? LiveHtml(live, lang)
                                    : KnifeHtml(knife, h.team, lang);
    }
    if (html.empty()) continue;

    // Same HTML: resend every hud_resend_ms (default 0 = every tick; CS2's center panel ignores
    // the event's duration and only stays steady when re-sent every frame, docs/HUD.md).
    // Changed HTML: send right away.
    auto& s = g_sent[slot];
    const bool changed = (s.html != html);
    if (s.html == html && (now - s.at) < std::chrono::milliseconds(Cfg().hud_resend_ms)) continue;
    // HUD tests are one-off cards (over another plugin's HUD); the ready HUD is the HUD level.
    const int sent = SendCenterHtml(slot, html, Cfg().hud_duration_s,
                                    t != tests.end() ? RU_HTML_PRIO_NOTICE : RU_HTML_PRIO_HUD);
    if (sent < 0) continue;  // another plugin's higher panel (download bar) is up: retry next tick
    const bool ok = sent == 1;
    g_sendOk.store(ok, std::memory_order_relaxed);
    if (ok) {
      s.html = html;
      s.at = now;
      if (t == tests.end()) g_showing.store(true, std::memory_order_relaxed);
      if (t != tests.end() && changed) {
        Debug("ready-hud: hudtest %d -> slot=%d (%zu bytes html)\n", t->second, slot, html.size());
      }
      if (!s_loggedSize.exchange(true)) Debug("ready-hud: first panel sent (%zu bytes html)\n", html.size());
      static std::atomic<bool> s_warnedBig{false};
      if (html.size() > 1000 && !s_warnedBig.exchange(true)) {
        Print("ready-hud: panel is %zu bytes (large; the client may cut it off)\n", html.size());
      }
    } else {
      g_showing.store(false, std::memory_order_relaxed);
      if (!s_warned.exchange(true)) PrintLine("ready-hud: per-client center HTML unavailable; ready HUD not shown.");
    }
  }
  for (auto it = g_sent.begin(); it != g_sent.end();) {
    if (present.count(it->first) == 0) it = g_sent.erase(it);
    else ++it;
  }
}

}  // namespace readyup
