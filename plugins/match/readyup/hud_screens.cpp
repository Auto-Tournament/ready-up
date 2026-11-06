// The clickable Panorama HUD for the match flow (hud_screens.h).
#include "readyup/hud_screens.h"
#include "readyup/text.h"
#include "readyup/i18n_api.h"

#include "readyup/hud_iface.h"
#include "readyup/plugin_api.h"

#include "readyup/admin_check.h"
#include "readyup/config.h"
#include "readyup/engine.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/match_features.h"
#include "readyup/match_state.h"
#include "readyup/match_stats.h"
#include "readyup/modes.h"
#include "readyup/pause_state.h"
#include "readyup/players.h"
#include "readyup/votes.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace readyup {
namespace {

constexpr double kMenuSeconds = 30.0;

struct Menu {
  std::string which;  // help | votes | info | cmds | admin | maps | rounds | endwho | end:<winner>
  uint64_t steamid64 = 0;
  double until = 0;
};
std::map<int, Menu> g_menus;              // slot -> open menu (game thread)
std::map<int, std::string> g_dismissed;   // slot -> vote the player said no to ("gg:<deadline>")
std::set<int> g_shown;                    // slots that got a screen last tick
struct Notice {
  std::string spec;
  double until = 0;
  HudSpecBuild build;  // set: the spec in the viewer's language
};
std::mutex g_noticeMu;
std::map<int, Notice> g_notices;          // slot -> card shown over the base screen
Notice g_noticeAll;                       // shown to everyone (or one side) over the base screen
int g_noticeAllTeam = 0;                  // 0 everyone, else the CS side it is for

const ru_hud_v1* Hud() {
  const ru_api* a = host::Api();
  if (!a) return nullptr;
  const auto* h = static_cast<const ru_hud_v1*>(a->get_interface(a->self, RU_HUD_IFACE_NAME, 1));
  if (!h || h->struct_size < offsetof(ru_hud_v1, screen) + sizeof(h->screen) || !h->screen) return nullptr;
  return h;
}

std::string Line(const char* k, const std::string& v) { return std::string(k) + "=" + v + "\n"; }
std::string Btn(const std::string& label, const char* tone, const char* state, const std::string& cmd) {
  return "btn=" + HudSpecText(label) + "|" + tone + "|" + state + "|" + cmd + "\n";
}

std::string TeamName(WebhookTeam t, const std::string& lang) {
  const auto ctx = WebhookGetMatchContext();
  if (ctx) {
    const std::string& n = t == WebhookTeam::Team1 ? ctx->team1_name : ctx->team2_name;
    if (!n.empty()) return n;
  }
  return text::ForLang(lang, t == WebhookTeam::Team1 ? "card.team1" : "card.team2");
}

// "de_dust2" -> "Dust2", workshop paths keep their last part.
std::string MapLabel(std::string m) {
  const size_t slash = m.find_last_of('/');
  if (slash != std::string::npos) m = m.substr(slash + 1);
  for (const char* pre : {"de_", "cs_", "ar_"}) {
    if (m.rfind(pre, 0) == 0) {
      m = m.substr(3);
      break;
    }
  }
  if (!m.empty() && m[0] >= 'a' && m[0] <= 'z') m[0] = static_cast<char>(m[0] - 'a' + 'A');
  return m;
}

bool Warmup() {
  const ReadyUpMode m = GetMode();
  return m == ReadyUpMode::ScrimWarmup || m == ReadyUpMode::MatchWarmup;
}

std::string MenuSpec(const Menu& m, int csTeam, const std::string& lang) {
  auto T = [&lang](const char* key, const text::Args& args = {}) { return text::ForLang(lang, key, args); };
  const bool admin = IsReadyUpAdmin(m.steamid64);
  const bool playing = csTeam == 2 || csTeam == 3;
  const std::string back = Line("close", T("menu.back") + "|#menu help") + Line("mouse", "on");
  const std::string adminBack = Line("close", T("menu.back") + "|#menu admin") + Line("mouse", "on");
  std::string s;
  if (m.which == "help") {
    s = Line("id", "help") + Line("kind", T("menu.help")) + Line("title", T("menu.what_need"));
    if (Warmup()) {
      if (playing) s += IsReady(m.steamid64) ? Btn(T("menu.unready"), "ghost", "", ".ur") : Btn(T("menu.ready"), "green", "", ".r");
    } else if (playing) {
      s += PauseStateGet().paused ? Btn(T("menu.unpause"), "warn", "", ".up") : Btn(T("menu.tech_pause"), "ghost", "", ".p");
      s += Btn(T("menu.votes"), "ghost", "", "#menu votes");
    }
    s += Btn(T("menu.call_admin"), "ghost", "", ".admin");
    s += Btn(T("menu.match_info"), "ghost", "", "#menu info") + Btn(T("menu.commands"), "ghost", "", "#menu cmds");
    if (admin) s += Btn(T("menu.admin"), "ghost", "", "#menu admin");
    s += Line("cols", "3") + Line("bw", "m");
  } else if (m.which == "votes") {
    s = Line("id", "votes") + Line("kind", T("menu.help") + " \xC2\xB7 " + T("menu.votes_lc")) + Line("title", T("menu.start_vote"));
    s += Btn(T("menu.give_up"), "danger", "", ".gg") + Btn(T("menu.replay_round"), "ghost", "", ".stop") + Btn(T("menu.timeout"), "ghost", "", ".tac");
    return s + Line("cols", "3") + Line("bw", "m") + back;
  } else if (m.which == "info") {
    const auto ctx = WebhookGetMatchContext();
    const auto ms = MatchStateGet();
    s = Line("id", "info") + Line("kind", T("menu.help") + " \xC2\xB7 " + T("menu.match_lc"));
    if (ctx) {
      s += Line("title", HudSpecText(TeamName(WebhookTeam::Team1, lang) + " vs " + TeamName(WebhookTeam::Team2, lang)));
      const int maps = std::max(1, ctx->num_maps);
      s += Line("line", T("menu.format") + "|" + HudSpecText(T("menu.format_value", {{"maps", std::to_string(maps)}, {"rounds", std::to_string(ctx->maxRounds)}})));
      std::string list;
      for (size_t i = 0; i < ctx->maplist.size() && i < 5; ++i) list += (i ? " \xE2\x86\x92 " : "") + MapLabel(ctx->maplist[i]);
      if (!list.empty()) s += Line("line", T("menu.maps") + "|" + HudSpecText(list));
      if (maps > 1) s += Line("line", T("menu.map") + "|" + HudSpecText(T("menu.map_of", {{"number", std::to_string(ms.map_number)}, {"maps", std::to_string(maps)}})));
    } else {
      s += Line("title", T("menu.scrim"));
      if (!ms.current_map.empty()) s += Line("line", T("menu.map") + "|" + HudSpecText(MapLabel(ms.current_map)));
    }
    if (!Warmup()) s += Line("line", T("menu.score") + "|" + std::to_string(ms.team1_score) + " - " + std::to_string(ms.team2_score));
    return s + back;
  } else if (m.which == "cmds") {
    s = Line("id", "cmds") + Line("kind", T("menu.help") + " \xC2\xB7 " + T("menu.commands_lc")) + Line("title", T("menu.prefer_typing"));
    s += Line("line", T("menu.ready") + "|.r") + Line("line", T("menu.tech_pause") + "|.p") + Line("line", T("menu.timeout") + "|.tac") +
         Line("line", T("menu.give_up") + "|.gg") + Line("line", T("menu.call_admin") + "|.admin");
    return s + back;
  } else if (m.which == "admin" && admin) {
    s = Line("id", "admin") + Line("kind", T("menu.admin")) + Line("title", T("menu.match_controls"));
    s += Btn(T("menu.pause"), "ghost", "", ".fp") + Btn(T("menu.unpause"), "ghost", "", ".fup");
    s += Btn(T("menu.replay_a_round"), "ghost", "", "#menu rounds") + Btn(T("menu.change_map"), "ghost", "", "#menu maps");
    s += Btn(T("menu.swap_teams"), "ghost", "", ".ru match swap") + Btn(T("menu.force_ready"), "ghost", "", ".forceready");
    s += Btn(T("menu.restart_map"), "ghost", "", ".restart") + Btn(T("menu.end_match"), "danger", "", "#menu endwho");
    s += Line("cols", "4") + Line("bw", "m");
  } else if (m.which == "maps" && admin) {
    s = Line("id", "maps") + Line("kind", T("menu.admin") + " \xC2\xB7 " + T("menu.change_map_lc")) + Line("title", T("menu.pick_map"));
    for (const char* map : {"mirage", "inferno", "nuke", "ancient", "anubis", "dust2", "train", "overpass"}) {
      s += Btn(MapLabel(map), "ghost", "", std::string(".map de_") + map);
    }
    s += Line("cmd", T("menu.other_map") + "|.map " + T("menu.name_or_id"));
    return s + Line("cols", "4") + Line("bw", "s") + adminBack;
  } else if (m.which == "rounds" && admin) {
    const auto ms = MatchStateGet();
    const int cur = std::max(1, ms.team1_score + ms.team2_score + 1);
    s = Line("id", "rounds") + Line("kind", T("menu.admin") + " \xC2\xB7 " + T("menu.replay_lc")) + Line("title", T("menu.which_round"));
    // Each round with the score it starts at: people remember "it was 7-6", not "round 14".
    std::vector<stats::RoundSummary> played;
    {
      std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
      played = stats::Current().Snapshot().rounds;
    }
    auto scoreBefore = [&](int round) {
      int t1 = 0, t2 = 0;
      for (const auto& rs : played) {
        if (rs.round_number >= round) break;
        t1 = rs.team1_score;
        t2 = rs.team2_score;
      }
      return std::to_string(t1) + "-" + std::to_string(t2);
    };
    for (int r = cur; r > 0 && r > cur - 6; --r) {
      s += "btn=" + HudSpecText(T("menu.round_n", {{"round", std::to_string(r)}})) + "|ghost||.ru match restore " + std::to_string(r) + "|" + scoreBefore(r) + "\n";
    }
    return s + Line("cols", "3") + Line("bw", "m") + adminBack;
  } else if (m.which == "endwho" && admin) {
    s = Line("id", "endwho") + Line("color", "ban") + Line("kind", T("menu.admin") + " \xC2\xB7 " + T("menu.end_match_lc")) + Line("title", T("menu.who_won"));
    s += Btn(TeamName(WebhookTeam::Team1, lang), "ghost", "", "#menu end:team1") + Btn(TeamName(WebhookTeam::Team2, lang), "ghost", "", "#menu end:team2");
    s += Btn(T("menu.no_winner"), "ghost", "", "#menu end:none");
    return s + Line("cols", "3") + Line("bw", "m") + adminBack;
  } else if (m.which.rfind("end:", 0) == 0 && admin) {
    const std::string w = m.which.substr(4);
    const std::string title = w == "none" ? T("menu.end_no_winner") : T("menu.team_wins_q", {{"team", TeamName(w == "team1" ? WebhookTeam::Team1 : WebhookTeam::Team2, lang)}});
    s = Line("id", "end") + Line("color", "ban") + Line("kind", T("menu.admin") + " \xC2\xB7 " + T("menu.end_match_lc")) + Line("title", HudSpecText(title)) +
        Line("sub", T("menu.ends_now"));
    s += Btn(T("menu.end_match"), "danger", "", ".endmatch " + w) + Btn(T("menu.cancel"), "ghost", "", "#menu endwho");
    return s + Line("bw", "m") + Line("mouse", "on");
  } else {
    return {};
  }
  return s + Line("close", T("menu.close") + "|#close") + Line("mouse", "on");
}

// The vote that concerns this player, if any ("" = none).
std::string VoteSpec(int slot, uint64_t steamid64, int csTeam, const std::string& lang) {
  auto T = [&lang](const char* key, const text::Args& args = {}) { return text::ForLang(lang, key, args); };
  if (GetMode() != ReadyUpMode::MatchLive || (csTeam != 2 && csTeam != 3)) return {};
  const WebhookTeam team = VotesTeamForSide(csTeam);
  if (team == WebhookTeam::Unknown) return {};
  const VotesHudInfo v = VotesHud(team, steamid64);
  const auto dis = g_dismissed.find(slot);
  if (v.gg) {
    const std::string key = "gg";
    if (dis == g_dismissed.end() || dis->second != key || v.ggVoted) {
      std::string s = Line("id", "gg") + Line("color", "ban") + Line("kind", T("vote.gg_kind")) +
                      Line("title", T("vote.gg_title")) + Line("timer", std::to_string(v.ggSecondsLeft)) +
                      Line("counter", HudSpecText(T("vote.gg_counter", {{"votes", std::to_string(v.ggCount)}, {"need", std::to_string(v.ggNeed)}}))) + Line("bw", "s");
      if (v.ggVoted) s += Btn(T("vote.yes"), "danger", "picked", "") + Btn(T("vote.no"), "ghost", "off", "");
      else s += Btn(T("vote.yes"), "danger", "", ".gg") + Btn(T("vote.no"), "ghost", "", "#dismiss gg") + Line("mouse", "on");
      return s;
    }
  } else if (dis != g_dismissed.end() && dis->second == "gg") {
    g_dismissed.erase(dis);
  }
  if (v.stop) {
    if (v.stopTeam == team) {
      return Line("id", "stopwait") + Line("kind", T("vote.stop_kind")) + Line("title", HudSpecText(T("vote.waiting_for", {{"team", TeamName(team == WebhookTeam::Team1 ? WebhookTeam::Team2 : WebhookTeam::Team1, lang)}}))) +
             Line("timer", std::to_string(v.stopSecondsLeft));
    }
    const auto d = g_dismissed.find(slot);
    if (d == g_dismissed.end() || d->second != "stop") {
      return Line("id", "stop") + Line("kind", T("vote.stop_kind")) +
             Line("title", HudSpecText(T("vote.wants_replay", {{"team", TeamName(v.stopTeam, lang)}}))) +
             Line("timer", std::to_string(v.stopSecondsLeft)) + Line("bw", "m") + Btn(T("vote.accept"), "green", "", ".stop") +
             Btn(T("vote.decline"), "ghost", "", "#dismiss stop") + Line("mouse", "on");
    }
  } else {
    const auto d = g_dismissed.find(slot);
    if (d != g_dismissed.end() && d->second == "stop") g_dismissed.erase(d);
  }
  return {};
}

// hud.so owns `.menu` and asks each plugin whose menu fits (ru_hud_v1 register_menu). Ours fits
// whenever this plugin runs the server (not practice, not an external flow); between rounds it opens
// the admin menu for admins, the help menu for everyone else.
const ru_hud_v1* g_menuHud = nullptr;

int MenuActive(void*, int) {
  const ReadyUpMode m = GetMode();
  return m != ReadyUpMode::Practice && m != ReadyUpMode::External;
}

void MenuOpen(void*, int slot, uint64_t steamid64) {
  if (HudScreensOpenMenu(slot, steamid64, IsReadyUpAdmin(steamid64) ? "admin" : "help")) return;
  if (slot >= 0) text::ChatSlot(slot, "reply.menu_between_rounds");
}

void EnsureMenuProvider(const ru_hud_v1* hud) {
  if (hud == g_menuHud) return;
  if (hud->struct_size < offsetof(ru_hud_v1, register_menu) + sizeof(hud->register_menu) || !hud->register_menu) return;
  hud->register_menu("match", 0, &MenuActive, &MenuOpen, nullptr);
  g_menuHud = hud;
}

void OnClick(void*, int slot, const char* command) {
  const std::string c = command ? command : "";
  if (c.rfind("#menu ", 0) == 0) {
    auto it = g_menus.find(slot);
    if (it == g_menus.end()) return;
    it->second.which = c.substr(6);
    it->second.until = host::NowSeconds() + kMenuSeconds;
    return;  // drawn on the next tick
  }
  if (c == "#close") {
    g_menus.erase(slot);
    return;
  }
  if (c.rfind("#dismiss ", 0) == 0) {
    g_dismissed[slot] = c.substr(9);
    return;
  }
  g_menus.erase(slot);  // a menu closes once something is picked
  const ru_api* a = host::Api();
  if (a && RU_API_HAS(a, run_chat_command)) {
    Debug("hud: slot %d clicked \"%s\"\n", slot, c.c_str());
    a->run_chat_command(a->self, slot, c.c_str());
  }
}

}  // namespace

HudSpecBuild NoticeSpec(std::string head, std::string kindKey, std::string titleKey, std::string subKey, i18n::Args args) {
  return [=](const std::string& lang) {
    std::string s = head + "kind=" + HudSpecText(text::ForLang(lang, kindKey, args)) + "\ntitle=" +
                    HudSpecText(text::ForLang(lang, titleKey, args)) + "\n";
    if (!subKey.empty()) s += "sub=" + HudSpecText(text::ForLang(lang, subKey, args)) + "\n";
    return s;
  };
}

std::string HudSpecText(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    if (ch == '\n' || ch == '\r') out += ' ';
    else if (ch != '|') out += ch;
  }
  return out;
}

bool HudScreensOn() {
  if (!Cfg().hud_screens) return false;
  const ru_hud_v1* h = Hud();
  return h && h->available();
}

bool HudMidRound() {
  return GetMode() == ReadyUpMode::MatchLive && !MatchFeaturesInFreeze() && !PauseStateGet().paused;
}

void HudScreensRun(const std::function<std::string(const HumanIdentity&)>& baseFor) {
  const ru_hud_v1* hud = Hud();
  if (!hud) return;
  EnsureMenuProvider(hud);
  const double now = host::NowSeconds();
  const bool mid = HudMidRound();
  std::set<int> present;
  for (const auto& h : ListHumans()) {
    // The log `<N>` is the slot in CS2: a player who just joined is drawn at once, not after the
    // first event that names their slot (the warmup card came seconds late, 2026-10-27).
    const int slot = h.slot >= 0 ? h.slot : h.userid;
    if (h.steamid64 == 0 || slot < 0 || slot > 63) continue;
    present.insert(slot);
    std::string spec;
    auto m = g_menus.find(slot);
    if (m != g_menus.end() && (now > m->second.until || mid || m->second.steamid64 != h.steamid64)) {
      g_menus.erase(m);
      m = g_menus.end();
    }
    const std::string lang = i18n::LanguageOf(host::Api(), slot);
    if (m != g_menus.end()) spec = MenuSpec(m->second, h.team, lang);
    if (spec.empty()) spec = VoteSpec(slot, h.steamid64, h.team, lang);
    if (spec.empty()) {
      std::lock_guard<std::mutex> lk(g_noticeMu);
      auto n = g_notices.find(slot);
      if (n != g_notices.end() && now > n->second.until) {
        g_notices.erase(n);
      } else if (n != g_notices.end()) {
        spec = n->second.build ? n->second.build(lang) : n->second.spec;
      }
      if (spec.empty() && now <= g_noticeAll.until && (g_noticeAllTeam == 0 || g_noticeAllTeam == h.team)) {
        spec = g_noticeAll.build ? g_noticeAll.build(lang) : g_noticeAll.spec;
      }
    }
    if (spec.empty()) spec = baseFor(h);
    if (!spec.empty() && mid) spec += "chips=1\n";
    hud->screen(slot, spec.c_str(), &OnClick, nullptr);
  }
  for (int slot : g_shown) {
    if (!present.count(slot)) {
      hud->screen(slot, "", nullptr, nullptr);
      g_menus.erase(slot);
      g_dismissed.erase(slot);
    }
  }
  g_shown.swap(present);
}

void HudScreensNotice(int slot, const std::string& spec, double seconds) {
  if (slot < 0 || slot > 63 || spec.empty()) return;
  std::lock_guard<std::mutex> lk(g_noticeMu);
  g_notices[slot] = Notice{spec, host::NowSeconds() + seconds, {}};
}

void HudScreensNotice(int slot, HudSpecBuild build, double seconds) {
  if (slot < 0 || slot > 63 || !build) return;
  std::lock_guard<std::mutex> lk(g_noticeMu);
  g_notices[slot] = Notice{{}, host::NowSeconds() + seconds, std::move(build)};
}

void HudScreensNoticeAll(const std::string& spec, double seconds, int csTeam) {
  if (spec.empty()) return;
  std::lock_guard<std::mutex> lk(g_noticeMu);
  g_noticeAll = Notice{spec, host::NowSeconds() + seconds, {}};
  g_noticeAllTeam = csTeam;
}

void HudScreensNoticeAll(HudSpecBuild build, double seconds, int csTeam) {
  if (!build) return;
  std::lock_guard<std::mutex> lk(g_noticeMu);
  g_noticeAll = Notice{{}, host::NowSeconds() + seconds, std::move(build)};
  g_noticeAllTeam = csTeam;
}

void HudScreensUninstall() {
  if (const ru_hud_v1* hud = Hud(); hud && hud == g_menuHud) hud->register_menu("match", 0, nullptr, nullptr, nullptr);
  g_menuHud = nullptr;
}

bool HudScreensOpenMenu(int slot, uint64_t steamid64, const std::string& which) {
  if (slot < 0 || slot > 63 || !HudScreensOn() || HudMidRound()) return false;
  const ReadyUpMode mode = GetMode();
  if (mode == ReadyUpMode::Practice || mode == ReadyUpMode::External) return false;
  if (which == "admin" && !IsReadyUpAdmin(steamid64)) return false;
  g_menus[slot] = Menu{which, steamid64, host::NowSeconds() + kMenuSeconds};
  return true;
}

}  // namespace readyup
