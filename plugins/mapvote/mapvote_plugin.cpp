// readyup-mapvote: map voting for community servers (movement, deathmatch, the skins tryout
// server, practice). Never while a match is loaded: the match plugin owns the map then.
//
//   .rtv                 rock the vote: when enough players (rtv_ratio of the humans) typed it, a
//                        vote starts; the winning map loads right after
//   .unrtv               take your .rtv back
//   .nominate <map>      put a map on the next ballot (one per player); `.nominate` alone lists
//                        the pool
//   .maps                the map pool
//   .nextmap             the map that comes next, once a vote picked it
//   .timeleft            time left on this map (map_time_minutes)
//   .1 .. .9             vote on the open ballot (the center panel shows the numbers; with the
//                        Auto Tournament HUD the ballot is buttons)
//   .menu                (hud.so) change map, nominate from the pool; the ballot while it is open
//
// With map_time_minutes, a vote starts vote_before_end_seconds before the time runs out; its
// winner loads when the time is up ("Extend" adds extend_minutes, max_extends times).
//
// Admin / console: `ru mapvote start|cancel|status|reload`, `ru mapvote change <map>`.
//
// Settings: cfg/ReadyUp/mapvote.cfg (see the file), re-read on every map start and with
// `ru mapvote reload`. The pool is `maps=` there; the platform is meant to push it later.
// Maps load through essentials (readyup.essentials.v1 load_map: the change card and the
// Workshop download bar), else plain changelevel / host_workshop_map.
#include "mapvote_rules.h"

#include "readyup/essentials_iface.h"
#include "readyup/help_iface.h"
#include "readyup/hud_iface.h"
#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#ifndef MAPVOTE_VERSION
#define MAPVOTE_VERSION "dev"
#endif

namespace mapvote {
namespace {

const ru_api* g_api = nullptr;
std::mt19937 g_rng{std::random_device{}()};

int Rng(int n) { return n <= 1 ? 0 : std::uniform_int_distribution<int>(0, n - 1)(g_rng); }

struct Vote {
  bool open = false;
  bool endOfMap = false;           // started by the time limit (the winner waits for the time to run out)
  std::vector<std::string> options;  // map entries; "" = Extend
  int extendIndex = -1;
  std::map<uint64_t, int> votes;
  double endsAt = 0, nextHud = 0;
};

struct State {
  Settings s;
  std::string currentMap;
  double mapStartedAt = 0;
  int extends = 0;
  std::vector<std::string> recent;                          // newest first
  std::set<uint64_t> rtv;
  std::vector<std::pair<uint64_t, std::string>> nominations;  // (player, entry)
  Vote vote;
  bool voted = false;          // this map's vote is done (rtv or end of map)
  std::string nextMap;         // the vote's winner
  double changeAt = 0;         // load nextMap at this time (0 = not scheduled)
};
State g;

// ---- helpers ----------------------------------------------------------------------------------

void ChatAll(const std::string& m) { g_api->chat_all(g_api->self, m.c_str(), 0); }
void Reply(const ru_command_ctx* c, const std::string& m) {
  if (c->is_console || c->slot < 0) ru_logf(g_api, RU_LOG_INFO, "%s", m.c_str());
  else g_api->chat_to_slot(g_api->self, c->slot, m.c_str());
}

const ru_essentials_v1* Essentials() {
  const auto* e = static_cast<const ru_essentials_v1*>(g_api->get_interface(g_api->self, RU_ESSENTIALS_IFACE_NAME, 1));
  return e && RU_API_HAS(e, load_map) && e->load_map ? e : nullptr;
}

// Off while the match plugin runs a match (or a scrim / knife round): the map is the match's.
bool MatchOwnsMap() {
  const auto* m = static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
  if (!m || !RU_API_HAS(m, mode) || !m->mode) return false;
  const char* mode = m->mode();
  const std::string s = mode ? mode : "";
  return s.rfind("match", 0) == 0 || s.rfind("knife", 0) == 0 || s.rfind("scrim", 0) == 0;
}

bool Active() { return g.s.enabled && !g.s.maps.empty() && !MatchOwnsMap(); }

int Humans() {
  int n = 0;
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) {
        if (!p->is_bot && p->connected && p->steamid64 != 0) ++*static_cast<int*>(u);
        return 1;
      },
      &n);
  return n;
}

void LoadSettings() {
  std::vector<std::string> warnings;
  g.s = mapvote::LoadSettings(
      [](const char* key, std::string* out) {
        char buf[8192] = {};
        const int n = g_api->config_get(g_api->self, key, buf, sizeof(buf));
        if (n < 0) return false;
        *out = buf;
        return true;
      },
      &warnings);
  for (const auto& w : warnings) ru_logf(g_api, RU_LOG_WARN, "mapvote.cfg: %s", w.c_str());
  if (g.s.maps.empty()) g.s.maps = mapvote::DefaultPool(g.currentMap);
}

bool LoadMap(const std::string& entry) {
  if (const auto* e = Essentials()) return e->load_map(entry.c_str()) == 1;
  std::string cmd;
  if (entry.rfind("workshop/", 0) == 0) {
    const std::string rest = entry.substr(9);
    cmd = "host_workshop_map " + rest.substr(0, rest.find('/'));
  } else {
    cmd = "changelevel " + entry;
  }
  return g_api->server_command(g_api->self, cmd.c_str()) == 1;
}

// ---- the vote -----------------------------------------------------------------------------------

bool BallotAll();
void BallotClose();
bool HasHud();

std::string BallotHtml(double now) {
  std::vector<int> tally(g.vote.options.size(), 0);
  for (const auto& kv : g.vote.votes) {
    if (kv.second >= 0 && kv.second < static_cast<int>(tally.size())) ++tally[static_cast<size_t>(kv.second)];
  }
  const int left = std::max(0, static_cast<int>(g.vote.endsAt - now + 0.99));
  std::string h = "<font class='fontSize-l' color='#FFD34D'><b>Vote for the next map</b></font> <font color='#9AA4B2'>" +
                  std::to_string(left) + "s</font><br>";
  for (size_t i = 0; i < g.vote.options.size(); ++i) {
    const std::string name = g.vote.options[i].empty() ? "Extend this map" : MapDisplayName(g.vote.options[i]);
    h += "<font color='#7FD7FF'><b>." + std::to_string(i + 1) + "</b></font> " + name + " <font color='#9AA4B2'>(" +
         std::to_string(tally[i]) + ")</font><br>";
  }
  return h;
}

void StartVote(bool endOfMap, double now) {
  std::vector<std::string> noms;
  for (const auto& n : g.nominations) noms.push_back(n.second);
  std::vector<std::string> recent(g.recent.begin(),
                                  g.recent.begin() + std::min<size_t>(g.recent.size(), static_cast<size_t>(g.s.recentMapsExcluded)));
  g.vote = Vote{};
  g.vote.open = true;
  g.vote.endOfMap = endOfMap;
  g.vote.options = BuildBallot(g.s.maps, noms, g.currentMap, recent, g.s.voteOptions, &Rng);
  if (endOfMap && g.s.mapTimeMinutes > 0 && g.extends < g.s.maxExtends) {
    g.vote.extendIndex = static_cast<int>(g.vote.options.size());
    g.vote.options.push_back("");
  }
  if (g.vote.options.empty()) {
    g.vote.open = false;
    ChatAll("Map vote: no other maps in the pool.");
    return;
  }
  g.vote.endsAt = now + g.s.voteSeconds;
  if (!HasHud()) ChatAll("Map vote: type the number of the map you want (.1 to ." + std::to_string(g.vote.options.size()) + ").");
  ru_logf(g_api, RU_LOG_INFO, "vote started (%s), %zu option(s)", endOfMap ? "end of map" : "rtv", g.vote.options.size());
}

void FinishVote(double now) {
  const int w = Winner(static_cast<int>(g.vote.options.size()), g.vote.votes, g.vote.extendIndex, &Rng);
  const bool endOfMap = g.vote.endOfMap;
  const size_t votes = g.vote.votes.size();
  const std::vector<std::string> options = g.vote.options;
  g.vote = Vote{};
  g_api->center_html_release(g_api->self, -1);
  BallotClose();
  g.rtv.clear();
  if (w < 0) return;
  if (options[static_cast<size_t>(w)].empty()) {
    ++g.extends;
    g.voted = false;  // another end-of-map vote when the extended time runs low
    ChatAll("Map vote: this map is extended by " + std::to_string(g.s.extendMinutes) + " minutes.");
    return;
  }
  g.voted = true;
  g.nextMap = options[static_cast<size_t>(w)];
  g.nominations.clear();
  const std::string name = MapDisplayName(g.nextMap);
  if (endOfMap) {
    ChatAll("Map vote: next map is " + name + (votes == 0 ? " (nobody voted, picked at random)." : "."));
    // The map changes when the time runs out (OnTick).
  } else {
    ChatAll("Map vote: " + name + " wins" + (votes == 0 ? " (nobody voted, picked at random)" : "") + ". Changing map in 5 seconds.");
    g.changeAt = now + 5.0;
  }
}

void TryRtv(double now) {
  const int humans = Humans();
  const int need = RtvNeeded(humans, g.s.rtvRatio);
  if (static_cast<int>(g.rtv.size()) >= need && humans >= g.s.rtvMinPlayers && !g.vote.open) StartVote(false, now);
}

// ---- chat commands ------------------------------------------------------------------------------

double Now() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// readyup.help.mapvote: the `.help` line (essentials), none while map voting is off.
const char* HelpLineNow() {
  return Active() ? "Map vote: .rtv vote to change map (.unrtv) | .nominate <map> | .maps | .nextmap | .timeleft" : nullptr;
}
const ru_help_v1 g_help = {sizeof(ru_help_v1), &HelpLineNow};

bool CheckActive(const ru_command_ctx* c) {
  if (Active()) return true;
  Reply(c, MatchOwnsMap() ? "Map vote is off during a match." : g.s.enabled ? "Map vote is off on this server: no map pool (maps= in mapvote.cfg)."
                                                  : "Map vote is off on this server.");
  return false;
}

void OnRtv(void*, const ru_command_ctx* c) {
  if (!CheckActive(c) || c->steamid64 == 0) return;
  const double now = Now();
  if (g.voted && !g.nextMap.empty()) return Reply(c, "The next map is already picked: " + MapDisplayName(g.nextMap) + ".");
  if (g.vote.open) return Reply(c, "A map vote is running: type its number.");
  if (now - g.mapStartedAt < g.s.rtvDelaySeconds) {
    return Reply(c, "Rock the vote opens " + std::to_string(g.s.rtvDelaySeconds - static_cast<int>(now - g.mapStartedAt)) +
                        " seconds into the map.");
  }
  const int humans = Humans();
  if (humans < g.s.rtvMinPlayers) return Reply(c, "Rock the vote needs " + std::to_string(g.s.rtvMinPlayers) + " players.");
  const bool added = g.rtv.insert(c->steamid64).second;
  const int need = RtvNeeded(humans, g.s.rtvRatio);
  if (added) {
    ChatAll(std::string(c->name ? c->name : "Someone") + " wants to change the map (" + std::to_string(g.rtv.size()) + "/" +
            std::to_string(need) + ", type .rtv).");
  } else {
    Reply(c, "You already voted to change the map (" + std::to_string(g.rtv.size()) + "/" + std::to_string(need) + ").");
  }
  TryRtv(now);
}

void OnUnrtv(void*, const ru_command_ctx* c) {
  if (!CheckActive(c)) return;
  if (g.rtv.erase(c->steamid64)) Reply(c, "Your .rtv is taken back.");
  else Reply(c, "You had not typed .rtv.");
}

std::string PoolLine() {
  std::string l;
  for (const auto& m : g.s.maps) {
    if (!l.empty()) l += ", ";
    l += MapDisplayName(m);
  }
  return l;
}

void OnMaps(void*, const ru_command_ctx* c) {
  if (!CheckActive(c)) return;
  Reply(c, "Maps: " + PoolLine());
}

void OnNominate(void*, const ru_command_ctx* c) {
  if (!CheckActive(c) || c->steamid64 == 0) return;
  if (c->argc < 2) return Reply(c, "Type .nominate <map>. Maps: " + PoolLine());
  if (g.vote.open) return Reply(c, "A map vote is running already.");
  std::string typed;
  for (int i = 1; i < c->argc; ++i) typed += (i > 1 ? " " : "") + std::string(c->argv[i]);
  std::vector<std::string> amb;
  const std::string entry = FindMap(g.s.maps, typed, &amb);
  if (entry.empty()) {
    if (!amb.empty()) {
      std::string l;
      for (const auto& a : amb) l += (l.empty() ? "" : ", ") + a;
      return Reply(c, "More than one map matches: " + l + ".");
    }
    return Reply(c, "No map called " + typed + " here. Maps: " + PoolLine());
  }
  if (MapDisplayName(entry) == MapDisplayName(g.currentMap) || entry == g.currentMap) return Reply(c, "That is the current map.");
  for (const auto& n : g.nominations) {
    if (n.second == entry && n.first != c->steamid64) return Reply(c, MapDisplayName(entry) + " is nominated already.");
  }
  bool replaced = false;
  for (auto& n : g.nominations) {
    if (n.first == c->steamid64) {
      n.second = entry;
      replaced = true;
    }
  }
  if (!replaced) g.nominations.emplace_back(c->steamid64, entry);
  ChatAll(std::string(c->name ? c->name : "Someone") + " nominated " + MapDisplayName(entry) + ".");
}

void OnNextmap(void*, const ru_command_ctx* c) {
  if (!CheckActive(c)) return;
  Reply(c, g.nextMap.empty() ? "The next map is not picked yet." : "Next map: " + MapDisplayName(g.nextMap) + ".");
}

void OnTimeleft(void*, const ru_command_ctx* c) {
  if (!CheckActive(c)) return;
  const int left = SecondsLeft(g.s, g.mapStartedAt, g.extends, Now());
  Reply(c, left < 0 ? "This map has no time limit." : "Time left on this map: " + FormatClock(left) + ".");
}

void OnVoteNumber(void* user, const ru_command_ctx* c) {
  const int n = static_cast<int>(reinterpret_cast<intptr_t>(user));
  if (!g.vote.open || c->steamid64 == 0) return;  // not ours to answer: no vote running
  if (n < 1 || n > static_cast<int>(g.vote.options.size())) return Reply(c, "Pick 1 to " + std::to_string(g.vote.options.size()) + ".");
  g.vote.votes[c->steamid64] = n - 1;
  g.vote.nextHud = 0;  // show the new count now
  const std::string& o = g.vote.options[static_cast<size_t>(n - 1)];
  Reply(c, "You voted for " + (o.empty() ? std::string("extending this map") : MapDisplayName(o)) + ".");
}

// ---- the HUD (hud.so, readyup.hud.v1.2) ---------------------------------------------------------
//
// With the Auto Tournament HUD the ballot is buttons: click a map, see the counts move. `.menu`
// (when no higher menu fits, or the movement menu's Maps button) opens rtv, the next map and the
// pool to nominate from. Without the HUD everything stays chat and the center panel.

constexpr const char* kMenuOwner = "mapvote";
constexpr int kMapsPerPage = 6;
const ru_hud_v1* g_menuHud = nullptr;
std::map<int, std::string> g_menuPage;  // slot -> "home" | "pool <offset>"

const ru_hud_v1* MenuHud() {
  if (!g_api) return nullptr;
  const auto* h = static_cast<const ru_hud_v1*>(g_api->get_interface(g_api->self, RU_HUD_IFACE_NAME, 1));
  if (!h || h->struct_size < offsetof(ru_hud_v1, register_menu) + sizeof(h->register_menu) || !h->menu) return nullptr;
  return h;
}

bool HasHud() { return MenuHud() != nullptr; }

std::string MenuText(const std::string& s) {
  std::string out;
  for (char ch : s) out += (ch == '\n' || ch == '\r' || ch == '|') ? ' ' : ch;
  return out;
}
std::string MenuBtn(const std::string& label, const char* tone, const std::string& cmd, const std::string& sub = "",
                    const char* state = "") {
  return "btn=" + MenuText(label) + "|" + tone + "|" + state + "|" + cmd + (sub.empty() ? "" : "|" + MenuText(sub)) + "\n";
}

std::string BallotSpec(uint64_t steamid64, double now) {
  std::vector<int> tally(g.vote.options.size(), 0);
  for (const auto& kv : g.vote.votes) {
    if (kv.second >= 0 && kv.second < static_cast<int>(tally.size())) ++tally[static_cast<size_t>(kv.second)];
  }
  const auto mine = g.vote.votes.find(steamid64);
  const int picked = mine == g.vote.votes.end() ? -1 : mine->second;
  std::string s = "id=mapvote-ballot\ncolor=acc\nkind=Map vote\ntitle=Next map\ntimer=" +
                  std::to_string(std::max(0, static_cast<int>(g.vote.endsAt - now + 0.99))) + "\n";
  for (size_t i = 0; i < g.vote.options.size() && i < 8; ++i) {
    const std::string name = g.vote.options[i].empty() ? "Extend" : MapDisplayName(g.vote.options[i]);
    s += MenuBtn(name, "ghost", "#vote " + std::to_string(i + 1), std::to_string(tally[i]),
                 static_cast<int>(i) == picked ? "picked" : "");
  }
  // The cursor only until the player has voted: the map goes on under the ballot.
  return s + "cols=" + std::string(g.vote.options.size() > 4 ? "4" : "2") + "\nbw=m\nmouse=" + (picked < 0 ? "on" : "off") + "\n";
}

std::string HomeSpec(uint64_t steamid64) {
  const int left = SecondsLeft(g.s, g.mapStartedAt, g.extends, Now());
  const std::string title = !g.nextMap.empty() ? "Next: " + MapDisplayName(g.nextMap)
                            : left >= 0        ? FormatClock(left) + " left"
                                               : MapDisplayName(g.currentMap);
  std::string s = "id=mapvote-home\nkind=" + MenuText("Maps \xC2\xB7 " + MapDisplayName(g.currentMap)) + "\ntitle=" + MenuText(title) + "\n";
  const std::string bar = std::to_string(g.rtv.size()) + " / " + std::to_string(RtvNeeded(Humans(), g.s.rtvRatio));
  if (g.rtv.count(steamid64)) s += MenuBtn("Take back", "ghost", ".unrtv", bar, "picked");
  else s += MenuBtn("Change map", "green", ".rtv", bar, g.voted && !g.nextMap.empty() ? "off" : "");
  std::string nominated;
  for (const auto& n : g.nominations) {
    if (n.first == steamid64) nominated = MapDisplayName(n.second);
  }
  s += MenuBtn("Nominate", "ghost", "#pool 0", nominated);
  return s + "cols=2\nbw=l\nclose=Close|#close\nmouse=on\n";
}

std::string PoolSpec(uint64_t steamid64, int offset) {
  const int n = static_cast<int>(g.s.maps.size());
  if (offset >= n || offset < 0) offset = 0;
  const int to = std::min(n, offset + kMapsPerPage);
  std::string mine;
  for (const auto& nm : g.nominations) {
    if (nm.first == steamid64) mine = nm.second;
  }
  std::string s = "id=mapvote-pool-" + std::to_string(offset) + "\nkind=Maps \xC2\xB7 nominate\ntitle=Pick one\n";
  for (int i = offset; i < to; ++i) {
    const std::string& e = g.s.maps[static_cast<size_t>(i)];
    const bool current = e == g.currentMap || MapDisplayName(e) == MapDisplayName(g.currentMap);
    s += MenuBtn(MapDisplayName(e), "ghost", "#nom " + std::to_string(i), current ? "now" : "",
                 current ? "off" : e == mine ? "picked" : "");
  }
  for (int i = to; i < offset + kMapsPerPage; ++i) s += MenuBtn(" ", "ghost", "", "", "off");
  s += MenuBtn("Prev", "ghost", "#pool " + std::to_string(std::max(0, offset - kMapsPerPage)), "", offset == 0 ? "off" : "");
  s += MenuBtn("Next", "ghost", "#pool " + std::to_string(to), "", to >= n ? "off" : "");
  return s + "cols=4\nbw=m\nclose=Back|#home\nmouse=on\n";
}

void OnMenuClick(void*, int slot, const char* command);

void MenuDraw(int slot, uint64_t steamid64) {
  const ru_hud_v1* h = MenuHud();
  if (!h) return;
  std::string spec;
  if (g.vote.open) spec = BallotSpec(steamid64, Now());
  else if (g_menuPage[slot].rfind("pool ", 0) == 0) spec = PoolSpec(steamid64, std::atoi(g_menuPage[slot].c_str() + 5));
  else spec = HomeSpec(steamid64);
  h->menu(slot, kMenuOwner, spec.c_str(), &OnMenuClick, nullptr);
}

void MenuClose(int slot) {
  g_menuPage.erase(slot);
  if (const ru_hud_v1* h = MenuHud()) h->menu(slot, kMenuOwner, "", nullptr, nullptr);
}

void OnMenuClick(void*, int slot, const char* command) {
  const std::string c = command ? command : "";
  ru_player p{};
  if (!g_api->get_player(g_api->self, slot, &p) || p.steamid64 == 0) return MenuClose(slot);
  if (c == "#close") return MenuClose(slot);
  std::string line = c;
  if (c.rfind("#vote ", 0) == 0) {
    line = "." + c.substr(6);
  } else if (c.rfind("#nom ", 0) == 0) {
    const int i = std::atoi(c.c_str() + 5);
    if (i < 0 || i >= static_cast<int>(g.s.maps.size())) return;
    line = ".nominate " + MapDisplayName(g.s.maps[static_cast<size_t>(i)]);
    g_menuPage[slot] = "home";
  } else if (c == "#home" || c.rfind("#pool ", 0) == 0) {
    g_menuPage[slot] = c.substr(1);
    return MenuDraw(slot, p.steamid64);
  } else if (c.empty() || c[0] != '.') {
    return;
  }
  // The button's command through its chat handler, as if typed.
  std::vector<std::string> words;
  for (size_t a = 0; a < line.size();) {
    const size_t b = std::min(line.find(' ', a), line.size());
    if (b > a) words.push_back(line.substr(a, b - a));
    a = b + 1;
  }
  if (words.empty()) return;
  std::vector<const char*> argv;
  for (const std::string& w : words) argv.push_back(w.c_str());
  const ru_command_ctx ctx{sizeof(ru_command_ctx), p.steamid64, slot, 0, p.name, line.c_str(), static_cast<int>(argv.size()), argv.data()};
  const std::string& cmd = words[0];
  if (cmd == ".rtv") OnRtv(nullptr, &ctx);
  else if (cmd == ".unrtv") OnUnrtv(nullptr, &ctx);
  else if (cmd == ".nominate") OnNominate(nullptr, &ctx);
  else if (cmd.size() == 2 && cmd[1] >= '1' && cmd[1] <= '9') OnVoteNumber(reinterpret_cast<void*>(static_cast<intptr_t>(cmd[1] - '0')), &ctx);
  if (g.vote.open) return;  // the ballot redraws for everyone (the tick)
  MenuDraw(slot, p.steamid64);  // the new rtv count / nomination
}

// The ballot on every human's HUD; false when there is no HUD (the center panel then).
bool BallotAll() {
  if (!MenuHud()) return false;
  g_api->for_each_player(
      g_api->self,
      [](void*, const ru_player* p) {
        if (!p->is_bot && p->connected && p->steamid64 != 0 && p->slot >= 0) MenuDraw(p->slot, p->steamid64);
        return 1;
      },
      nullptr);
  return true;
}

void BallotClose() {
  if (const ru_hud_v1* h = MenuHud()) h->menu(RU_HUD_ALL, kMenuOwner, "", nullptr, nullptr);
  g_menuPage.clear();
}

int OnMenuActive(void*, int) { return Active() ? 1 : 0; }

void OnMenuOpen(void*, int slot, uint64_t steamid64) {
  g_menuPage[slot] = "home";
  MenuDraw(slot, steamid64);
}

void EnsureMenuProvider() {
  const ru_hud_v1* h = MenuHud();
  if (!h || h == g_menuHud) return;
  h->register_menu(kMenuOwner, 10, &OnMenuActive, &OnMenuOpen, nullptr);
  g_menuHud = h;
}

void MenuUninstall() {
  if (const ru_hud_v1* h = MenuHud(); h && h == g_menuHud) h->register_menu(kMenuOwner, 10, nullptr, nullptr, nullptr);
  g_menuHud = nullptr;
  g_menuPage.clear();
}

// ---- ru mapvote ---------------------------------------------------------------------------------

void OnRu(void*, const ru_command_ctx* c) {
  const std::string sub = c->argc > 2 ? c->argv[2] : "status";
  const bool admin = c->is_console || g_api->is_admin(g_api->self, c->steamid64);
  if (sub == "status") {
    const int left = SecondsLeft(g.s, g.mapStartedAt, g.extends, Now());
    return Reply(c, std::string("mapvote: ") + (Active() ? "on" : (MatchOwnsMap() ? "off (match)" : "off")) + ", " +
                        std::to_string(g.s.maps.size()) + " map(s), rtv " + std::to_string(g.rtv.size()) + "/" +
                        std::to_string(RtvNeeded(Humans(), g.s.rtvRatio)) + ", time left " +
                        (left < 0 ? std::string("none") : FormatClock(left)) + (g.nextMap.empty() ? "" : ", next " + MapDisplayName(g.nextMap)));
  }
  if (!admin) return Reply(c, "not authorized");
  if (sub == "reload") {
    LoadSettings();
    return Reply(c, "mapvote: settings reloaded, " + std::to_string(g.s.maps.size()) + " map(s).");
  }
  if (sub == "start") {
    if (!Active()) return Reply(c, "mapvote: off here.");
    if (g.vote.open) return Reply(c, "mapvote: a vote is running.");
    StartVote(false, Now());
    return;
  }
  if (sub == "cancel") {
    if (!g.vote.open && g.changeAt == 0) return Reply(c, "mapvote: nothing to cancel.");
    g.vote = Vote{};
    g.changeAt = 0;
    g.nextMap.clear();
    g.voted = false;
    g_api->center_html_release(g_api->self, -1);
    BallotClose();
    ChatAll("Map vote cancelled by an admin.");
    return;
  }
  if (sub == "change" && c->argc > 3) {
    std::vector<std::string> amb;
    std::string entry = FindMap(g.s.maps, c->argv[3], &amb);
    if (entry.empty()) entry = c->argv[3];  // any map an admin names
    g.nextMap = entry;
    g.voted = true;
    g.changeAt = Now() + 3.0;
    ChatAll("An admin is changing the map to " + MapDisplayName(entry) + ".");
    return;
  }
  Reply(c, "usage: ru mapvote [status|start|cancel|reload|change <map>]");
}

// ---- engine -------------------------------------------------------------------------------------

void OnMapStart(void*, const ru_event*) {
  const char* cm = g_api->current_map(g_api->self);
  const std::string map = cm ? cm : "";
  if (!g.currentMap.empty() && g.currentMap != map) {
    g.recent.insert(g.recent.begin(), g.currentMap);
    if (g.recent.size() > 20) g.recent.resize(20);
  }
  g.currentMap = map;
  g.mapStartedAt = Now();
  g.extends = 0;
  g.rtv.clear();
  g.nominations.clear();
  g.vote = Vote{};
  g.voted = false;
  g.nextMap.clear();
  g.changeAt = 0;
  LoadSettings();
}

void OnTick(void*, const ru_tick_info* t) {
  const double now = t->now;
  EnsureMenuProvider();
  if (g.vote.open) {
    if (!Active()) {
      g.vote = Vote{};
      g_api->center_html_release(g_api->self, -1);
      BallotClose();
      return;
    }
    if (now >= g.vote.endsAt) {
      FinishVote(now);
    } else if (now >= g.vote.nextHud) {
      g.vote.nextHud = now + 1.0;
      if (!BallotAll()) g_api->center_html_all_prio(g_api->self, BallotHtml(now).c_str(), 2, RU_HTML_PRIO_NOTICE);
    }
    return;
  }
  if (g.changeAt > 0 && now >= g.changeAt) {
    g.changeAt = 0;
    if (!g.nextMap.empty() && Active()) {
      if (!LoadMap(g.nextMap)) ru_logf(g_api, RU_LOG_WARN, "could not load %s", g.nextMap.c_str());
    }
    return;
  }
  if (!Active() || g.s.mapTimeMinutes <= 0) return;
  const int left = SecondsLeft(g.s, g.mapStartedAt, g.extends, now);
  if (!g.voted && left <= g.s.voteBeforeEndSeconds && Humans() > 0) StartVote(true, now);
  if (left == 0 && g.voted && !g.nextMap.empty() && g.changeAt == 0) {
    ChatAll("Time is up. Changing map to " + MapDisplayName(g.nextMap) + ".");
    g.changeAt = now + 3.0;
  }
}

void OnPlayerEvent(void*, const ru_event* e) {
  // A player who leaves takes their .rtv and nomination along; the rest may now reach the bar.
  if (!e || e->type != RU_EVENT_PLAYER_DISCONNECT) return;
  const uint64_t sid = e->steamid64;
  g.rtv.erase(sid);
  g.nominations.erase(std::remove_if(g.nominations.begin(), g.nominations.end(),
                                     [sid](const std::pair<uint64_t, std::string>& n) { return n.first == sid; }),
                      g.nominations.end());
  g.vote.votes.erase(sid);
  if (Active() && !g.vote.open && !g.rtv.empty()) TryRtv(Now());
}

}  // namespace
}  // namespace mapvote

using namespace mapvote;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 6u,  // needs API 1.6 (center panel priorities)
      "mapvote",
      MAPVOTE_VERSION,
      "Ready Up",
      "map voting: .rtv, .nominate, end-of-map vote, .timeleft (community servers; off during a match)",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, center_html_all_prio) ||
      !RU_API_HAS(api, current_map)) {
    return 1;
  }
  g_api = api;
  g = State{};
  g_menuHud = nullptr;
  const char* cm = api->current_map(api->self);
  g.currentMap = cm ? cm : "";
  LoadSettings();  // after the map: an empty pool follows it (DefaultPool)
  g.mapStartedAt = Now();
  struct Cmd {
    const char* name;
    ru_command_fn fn;
  };
  for (const Cmd& c : {Cmd{".rtv", &OnRtv}, Cmd{".unrtv", &OnUnrtv}, Cmd{".nominate", &OnNominate}, Cmd{".maps", &OnMaps},
                       Cmd{".nextmap", &OnNextmap}, Cmd{".timeleft", &OnTimeleft}}) {
    if (!api->register_chat_command(api->self, c.name, c.fn, nullptr)) {
      ru_logf(api, RU_LOG_WARN, "could not register `%s` (another plugin owns it)", c.name);
    }
  }
  for (int i = 1; i <= 9; ++i) {
    const std::string n = "." + std::to_string(i);
    if (!api->register_chat_command(api->self, n.c_str(), &OnVoteNumber, reinterpret_cast<void*>(static_cast<intptr_t>(i)))) {
      ru_logf(api, RU_LOG_WARN, "could not register `%s` (another plugin owns it)", n.c_str());
    }
  }
  if (!api->register_ru_subcommand(api->self, "mapvote", &OnRu, nullptr)) {
    ru_logf(api, RU_LOG_WARN, "could not register `ru mapvote` (another plugin owns it)");
  }
  api->subscribe(api->self, RU_EVENT_MAP_START, &OnMapStart, nullptr);
  api->subscribe(api->self, RU_EVENT_PLAYER_DISCONNECT, &OnPlayerEvent, nullptr);
  api->on_tick(api->self, &OnTick, nullptr);
  api->provide_interface(api->self, RU_HELP_IFACE_PREFIX "mapvote", RU_HELP_IFACE_VERSION,
                         const_cast<ru_help_v1*>(&g_help));
  ru_logf(api, RU_LOG_INFO, "loaded " MAPVOTE_VERSION ": %zu map(s)%s", g.s.maps.size(), g.s.enabled ? "" : " (disabled)");
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  if (g_api) MenuUninstall();  // hud.so's .menu stops asking us
  if (g_api && g.vote.open) g_api->center_html_release(g_api->self, -1);
  if (g_api) ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
