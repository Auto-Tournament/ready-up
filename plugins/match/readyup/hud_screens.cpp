// The clickable Panorama HUD for the match flow (hud_screens.h).
#include "readyup/hud_screens.h"

#include "readyup/hud_iface.h"
#include "readyup/plugin_api.h"

#include "readyup/admin_check.h"
#include "readyup/config.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/match_features.h"
#include "readyup/match_state.h"
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
};
std::mutex g_noticeMu;
std::map<int, Notice> g_notices;          // slot -> card shown over the base screen

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

std::string TeamName(WebhookTeam t) {
  const auto ctx = WebhookGetMatchContext();
  if (ctx) {
    const std::string& n = t == WebhookTeam::Team1 ? ctx->team1_name : ctx->team2_name;
    if (!n.empty()) return n;
  }
  return t == WebhookTeam::Team1 ? "Team 1" : "Team 2";
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

std::string MenuSpec(const Menu& m, int csTeam) {
  const bool admin = IsReadyUpAdmin(m.steamid64);
  const bool playing = csTeam == 2 || csTeam == 3;
  const std::string back = Line("close", "Back|#menu help") + Line("mouse", "on");
  const std::string adminBack = Line("close", "Back|#menu admin") + Line("mouse", "on");
  std::string s;
  if (m.which == "help") {
    s = Line("id", "help") + Line("kind", "Help") + Line("title", "What do you need?");
    if (Warmup()) {
      if (playing) s += IsReady(m.steamid64) ? Btn("Unready", "ghost", "", ".ur") : Btn("Ready", "green", "", ".r");
    } else if (playing) {
      s += PauseStateGet().paused ? Btn("Unpause", "warn", "", ".up") : Btn("Tech pause", "ghost", "", ".p");
      s += Btn("Votes", "ghost", "", "#menu votes");
    }
    s += Btn("Call an admin", "ghost", "", ".admin");
    s += Btn("Match info", "ghost", "", "#menu info") + Btn("Commands", "ghost", "", "#menu cmds");
    if (admin) s += Btn("Admin", "ghost", "", "#menu admin");
    s += Line("cols", "2") + Line("bw", "l");
  } else if (m.which == "votes") {
    s = Line("id", "votes") + Line("kind", "Help \xC2\xB7 votes") + Line("title", "Start a vote");
    s += Btn("Give up", "danger", "", ".gg") + Btn("Replay round", "ghost", "", ".stop") + Btn("Timeout", "ghost", "", ".tac");
    return s + Line("cols", "3") + Line("bw", "m") + back;
  } else if (m.which == "info") {
    const auto ctx = WebhookGetMatchContext();
    const auto ms = MatchStateGet();
    s = Line("id", "info") + Line("kind", "Help \xC2\xB7 match");
    if (ctx) {
      s += Line("title", HudSpecText(TeamName(WebhookTeam::Team1) + " vs " + TeamName(WebhookTeam::Team2)));
      const int maps = std::max(1, ctx->num_maps);
      s += Line("line", "Format|Best of " + std::to_string(maps) + " \xC2\xB7 " + std::to_string(ctx->maxRounds) + " rounds");
      std::string list;
      for (size_t i = 0; i < ctx->maplist.size() && i < 5; ++i) list += (i ? " \xE2\x86\x92 " : "") + MapLabel(ctx->maplist[i]);
      if (!list.empty()) s += Line("line", "Maps|" + HudSpecText(list));
      if (maps > 1) s += Line("line", "Map|" + std::to_string(ms.map_number) + " of " + std::to_string(maps));
    } else {
      s += Line("title", "Scrim");
      if (!ms.current_map.empty()) s += Line("line", "Map|" + HudSpecText(MapLabel(ms.current_map)));
    }
    if (!Warmup()) s += Line("line", "Score|" + std::to_string(ms.team1_score) + " - " + std::to_string(ms.team2_score));
    return s + back;
  } else if (m.which == "cmds") {
    s = Line("id", "cmds") + Line("kind", "Help \xC2\xB7 commands") + Line("title", "Prefer typing?");
    s += Line("line", "Ready|.r") + Line("line", "Tech pause|.p") + Line("line", "Timeout|.tac") +
         Line("line", "Give up|.gg") + Line("line", "Call an admin|.admin");
    return s + back;
  } else if (m.which == "admin" && admin) {
    s = Line("id", "admin") + Line("kind", "Admin") + Line("title", "Match controls");
    s += Btn("Pause", "ghost", "", ".fp") + Btn("Unpause", "ghost", "", ".fup");
    s += Btn("Replay a round", "ghost", "", "#menu rounds") + Btn("Change map", "ghost", "", "#menu maps");
    s += Btn("Swap teams", "ghost", "", ".ru match swap") + Btn("Force ready", "ghost", "", ".forceready");
    s += Btn("Restart map", "ghost", "", ".restart") + Btn("End match", "danger", "", "#menu endwho");
    s += Line("cols", "2") + Line("bw", "l");
  } else if (m.which == "maps" && admin) {
    s = Line("id", "maps") + Line("kind", "Admin \xC2\xB7 change map") + Line("title", "Pick a map");
    for (const char* map : {"mirage", "inferno", "nuke", "ancient", "anubis", "dust2", "train", "overpass"}) {
      s += Btn(MapLabel(map), "ghost", "", std::string(".map de_") + map);
    }
    s += Line("cmd", "Any other map or workshop id|.map <name or id>");
    return s + Line("cols", "4") + Line("bw", "s") + adminBack;
  } else if (m.which == "rounds" && admin) {
    const auto ms = MatchStateGet();
    const int cur = std::max(1, ms.team1_score + ms.team2_score + 1);
    s = Line("id", "rounds") + Line("kind", "Admin \xC2\xB7 replay") + Line("title", "Back to which round?");
    for (int r = cur; r > 0 && r > cur - 6; --r) s += Btn("Round " + std::to_string(r), "ghost", "", ".ru match restore " + std::to_string(r));
    return s + Line("cols", "3") + Line("bw", "s") + adminBack;
  } else if (m.which == "endwho" && admin) {
    s = Line("id", "endwho") + Line("color", "ban") + Line("kind", "Admin \xC2\xB7 end match") + Line("title", "Who won?");
    s += Btn(TeamName(WebhookTeam::Team1), "ghost", "", "#menu end:team1") + Btn(TeamName(WebhookTeam::Team2), "ghost", "", "#menu end:team2");
    s += Btn("No winner", "ghost", "", "#menu end:none");
    return s + Line("cols", "3") + Line("bw", "m") + adminBack;
  } else if (m.which.rfind("end:", 0) == 0 && admin) {
    const std::string w = m.which.substr(4);
    const std::string title = w == "team1" ? TeamName(WebhookTeam::Team1) + " wins?" : w == "team2" ? TeamName(WebhookTeam::Team2) + " wins?" : "End with no winner?";
    s = Line("id", "end") + Line("color", "ban") + Line("kind", "Admin \xC2\xB7 end match") + Line("title", HudSpecText(title)) +
        Line("sub", "Ends the match now. No undo.");
    s += Btn("End match", "danger", "", ".endmatch " + w) + Btn("Cancel", "ghost", "", "#menu endwho");
    return s + Line("bw", "m") + Line("mouse", "on");
  } else {
    return {};
  }
  return s + Line("close", "Close|#close") + Line("mouse", "on");
}

// The vote that concerns this player, if any ("" = none).
std::string VoteSpec(int slot, uint64_t steamid64, int csTeam) {
  if (GetMode() != ReadyUpMode::MatchLive || (csTeam != 2 && csTeam != 3)) return {};
  const WebhookTeam team = VotesTeamForSide(csTeam);
  if (team == WebhookTeam::Unknown) return {};
  const VotesHudInfo v = VotesHud(team, steamid64);
  const auto dis = g_dismissed.find(slot);
  if (v.gg) {
    const std::string key = "gg";
    if (dis == g_dismissed.end() || dis->second != key || v.ggVoted) {
      std::string s = Line("id", "gg") + Line("color", "ban") + Line("kind", "Give up vote") +
                      Line("title", "Give up this map?") + Line("timer", std::to_string(v.ggSecondsLeft)) +
                      Line("counter", std::to_string(v.ggCount) + " / " + std::to_string(v.ggNeed) + " yes") + Line("bw", "s");
      if (v.ggVoted) s += Btn("Yes", "danger", "picked", "") + Btn("No", "ghost", "off", "");
      else s += Btn("Yes", "danger", "", ".gg") + Btn("No", "ghost", "", "#dismiss gg") + Line("mouse", "on");
      return s;
    }
  } else if (dis != g_dismissed.end() && dis->second == "gg") {
    g_dismissed.erase(dis);
  }
  if (v.stop) {
    if (v.stopTeam == team) {
      return Line("id", "stopwait") + Line("kind", "Replay this round?") + Line("title", "Waiting for " + HudSpecText(TeamName(team == WebhookTeam::Team1 ? WebhookTeam::Team2 : WebhookTeam::Team1))) +
             Line("timer", std::to_string(v.stopSecondsLeft));
    }
    const auto d = g_dismissed.find(slot);
    if (d == g_dismissed.end() || d->second != "stop") {
      return Line("id", "stop") + Line("kind", "Replay this round?") +
             Line("title", HudSpecText(TeamName(v.stopTeam)) + " wants to replay it") +
             Line("timer", std::to_string(v.stopSecondsLeft)) + Line("bw", "m") + Btn("Accept", "green", "", ".stop") +
             Btn("Decline", "ghost", "", "#dismiss stop") + Line("mouse", "on");
    }
  } else {
    const auto d = g_dismissed.find(slot);
    if (d != g_dismissed.end() && d->second == "stop") g_dismissed.erase(d);
  }
  return {};
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
  const double now = host::NowSeconds();
  const bool mid = HudMidRound();
  std::set<int> present;
  for (const auto& h : ListHumans()) {
    const int slot = h.slot;
    if (h.steamid64 == 0 || slot < 0 || slot > 63) continue;
    present.insert(slot);
    std::string spec;
    auto m = g_menus.find(slot);
    if (m != g_menus.end() && (now > m->second.until || mid || m->second.steamid64 != h.steamid64)) {
      g_menus.erase(m);
      m = g_menus.end();
    }
    if (m != g_menus.end()) spec = MenuSpec(m->second, h.team);
    if (spec.empty()) spec = VoteSpec(slot, h.steamid64, h.team);
    if (spec.empty()) {
      std::lock_guard<std::mutex> lk(g_noticeMu);
      auto n = g_notices.find(slot);
      if (n != g_notices.end() && now > n->second.until) {
        g_notices.erase(n);
      } else if (n != g_notices.end()) {
        spec = n->second.spec;
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
  g_notices[slot] = Notice{spec, host::NowSeconds() + seconds};
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
