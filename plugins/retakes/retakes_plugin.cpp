// readyup-retakes: retakes for fun servers. Every round the bomb is already planted on one site;
// the Ts hold it, the CTs retake it.
//
// Spawns are CS2's own: maps/<map>_retake.txt, read from the server's pak01_dir.vpk at map start
// (retakes_rules.h). On a map without one the plugin waits (a chat line says so). Rounds:
//   round_prestart  the next teams (queue past max_players, scramble after a T streak, the CTs
//                   who scored attack after a CT win) and the team changes;
//   round_start     a site, everyone on that site's spawns, the planter on the most central T spawn;
//   freeze end      the bomb is planted where the planter stands (a planted_c4 entity).
//   round_poststart everyone's rifle (`.guns`, kept in <data_dir>/guns.txt), one AWP per team
//                   drawn from who wants it; Deagle, armor + helmet and kits are CS2's own cvars.
// HUD (hud.so, readyup.hud.v1; chat without it): the `.guns` menu, a centre panel in freeze time
// (Retake / Hold <site>, the players on each side), the queue card (at_kit #at_card) while waiting.
// While it runs, the match plugin is in external mode "retakes" (readyup.match.v1), so no scrim
// warmup or idle cfg gets in the way. A match load ends it.
//
// Console: `ru retakes` (status), `ru retakes site a|b|random`. Chat: `.guns`, `.guns ak|sg|m4|m4s|aug|awp`.
#include "retakes_rules.h"

#include "readyup/i18n.h"
#include "readyup/hud_iface.h"
#include "readyup/i18n_api.h"
#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef RETAKES_VERSION
#define RETAKES_VERSION "dev"
#endif

extern const readyup::i18n::Locale kReadyUpLocales[];
extern const size_t kReadyUpLocaleCount;

namespace {

const ru_api* g_api = nullptr;
std::string g_csgoDir;  // .../game/csgo
std::mt19937 g_rng{std::random_device{}()};
const retakes::Shuffle kShuffle = [](std::vector<size_t>& v) { std::shuffle(v.begin(), v.end(), g_rng); };

readyup::i18n::Catalog g_text;
using Args = readyup::i18n::Args;
void ChatAllKey(const char* key, const Args& args = {}) {
  readyup::i18n::ChatAll(g_api, 0, [&](const std::string& lang) { return g_text.Tr(lang, key, args); });
}
void ChatSlotKey(int slot, const char* key, const Args& args = {}) {
  const std::string text = g_text.Tr(readyup::i18n::LanguageOf(g_api, slot), key, args);
  g_api->chat_to_slot(g_api->self, slot, text.c_str());
}

// cfg/ReadyUp/retakes.cfg
struct Settings {
  bool enabled = true;
  int maxPlayers = 9;
  int scrambleAfter = 5;
} g_set;

// What CS2 needs for retakes; set at every map start while the mode runs.
const char* const kCvars[] = {
    // Competitive rules; CS2's own retakes mode (game_mode 5) must not run alongside.
    "game_type 0",                 "game_mode 1",
    "bot_quota 0",                 "mp_autoteambalance 0",       "mp_limitteams 0",
    "mp_give_player_c4 0",         "mp_halftime 0",              "mp_match_can_clinch 0",
    "mp_maxmoney 0",               "mp_startmoney 0",            "mp_playercashawards 0",
    "mp_teamcashawards 0",         "mp_buytime 0",               "mp_freezetime 1",
    "mp_roundtime_defuse 1.25",    "mp_c4timer 40",              "mp_solid_teammates 1",
    "mp_free_armor 2",             "mp_defuser_allocation 2",    "mp_respawn_on_death_ct 0",
    "mp_respawn_on_death_t 0",     "mp_ignore_round_win_conditions 0",
    "mp_join_grace_time 0",        "mp_maxrounds 30",            "mp_friendlyfire 0",
    "mp_round_restart_delay 2",    "mp_death_drop_gun 1",        "sv_skirmish_id 0",
    "mp_t_default_primary \"\"",                 "mp_ct_default_primary \"\"",  // .guns gives them
    "mp_t_default_secondary weapon_deagle",       "mp_ct_default_secondary weapon_deagle",
};

struct State {
  std::string map;
  retakes::MapSpawns spawns;
  bool spawnsOk = false;
  bool on = false;          // external mode "retakes" is ours
  int site = 0;             // this round: 0 = A, 1 = B
  int forcedSite = -1;      // ru retakes site a|b
  int lastWinner = 0;
  retakes::Streak streak;
  std::map<uint64_t, int> score;   // this round
  std::map<uint64_t, int> queued;  // id -> when they started waiting
  int queueClock = 0;
  float plantPos[3] = {0, 0, 0};
  bool plantPending = false;
  int round = 0;
} g;

std::map<uint64_t, retakes::Loadout> g_guns;  // <data_dir>/guns.txt
std::string g_gunsFile;
std::set<int> g_gunsOpen;    // slots with the .guns menu up
std::set<int> g_queueCards;  // slots with the queue card up

void Log(int level, const char* fmt, const char* a = "", const char* b = "") {
  char buf[512];
  std::snprintf(buf, sizeof buf, fmt, a, b);
  g_api->log(g_api->self, level, buf);
}

const ru_match_v1* Match() {
  return static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
}

uint64_t IdOf(const ru_player& p) { return p.steamid64 ? p.steamid64 : static_cast<uint64_t>(p.slot + 1); }

std::vector<ru_player> Players() {
  std::vector<ru_player> out;
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) {
        if (p->connected && p->slot >= 0) static_cast<std::vector<ru_player>*>(u)->push_back(*p);
        return 1;
      },
      &out);
  return out;
}

// ---- spawns from the game ----------------------------------------------------------------------

bool ReadAt(const std::string& path, uint64_t offset, uint32_t length, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  f.seekg(static_cast<std::streamoff>(offset));
  out->assign(length, '\0');
  return length == 0 || static_cast<bool>(f.read(&(*out)[0], length));
}

bool LoadSpawns(const std::string& map, std::string* why) {
  const std::string dir = g_csgoDir + "/pak01_dir.vpk";
  std::string header;
  if (!ReadAt(dir, 0, retakes::kVpkHeaderV2, &header)) return *why = "no pak01_dir.vpk", false;
  const uint32_t treeSize =
      retakes::VpkTreeSize(reinterpret_cast<const uint8_t*>(header.data()), header.size());
  std::string tree;
  if (!treeSize || !ReadAt(dir, retakes::kVpkHeaderV2, treeSize, &tree)) return *why = "not a VPK v2", false;
  retakes::VpkEntry e;
  if (!retakes::VpkFind(reinterpret_cast<const uint8_t*>(tree.data()), tree.size(),
                        "maps/" + map + "_retake.txt", &e))
    return *why = "no retake spawns for this map", false;
  char name[32];
  std::snprintf(name, sizeof name, "/pak01_%03u.vpk", static_cast<unsigned>(e.archive));
  const bool inDir = e.archive == retakes::kVpkInDir;
  std::string body;
  if (!ReadAt(inDir ? dir : g_csgoDir + name, inDir ? retakes::kVpkHeaderV2 + uint64_t{treeSize} + e.offset : e.offset,
              e.length, &body))
    return *why = "could not read the spawn file", false;
  std::string err;
  if (!retakes::ParseRetakeFile(e.preload + body, &g.spawns, &err)) return *why = err, false;
  if (!g.spawns.Usable()) return *why = "a site without CT or T spawns", false;
  return true;
}

// shortcut: the T spawn nearest the middle of a site's T spawns stands in for "inside the
// bombsite"; read func_bomb_target's bounds if a map puts its middle spawn off the site.
std::vector<int> Plantable(const retakes::SiteSpawns& s) {
  if (s.t.empty()) return {};
  float c[2] = {0, 0};
  for (const auto& sp : s.t) c[0] += sp.pos[0], c[1] += sp.pos[1];
  c[0] /= s.t.size(), c[1] /= s.t.size();
  int best = 0;
  float bestD = 1e30f;
  for (size_t i = 0; i < s.t.size(); ++i) {
    const float dx = s.t[i].pos[0] - c[0], dy = s.t[i].pos[1] - c[1], d = dx * dx + dy * dy;
    if (d < bestD) bestD = d, best = static_cast<int>(i);
  }
  return {best};
}

// ---- the bomb ----------------------------------------------------------------------------------

template <typename T>
void Put(void* base, int off, T v) {
  std::memcpy(static_cast<unsigned char*>(base) + off, &v, sizeof v);
}

void* GameRules(void** proxyOut) {
  const int rulesPtr = g_api->schema_offset(g_api->self, "CCSGameRulesProxy", "m_pGameRules");
  if (rulesPtr <= 0) return nullptr;
  for (int i = 0; i < 2048; ++i) {
    void* e = g_api->entity_by_index(g_api->self, i);
    const char* cls = e ? g_api->entity_classname(g_api->self, e) : nullptr;
    if (!cls || std::strcmp(cls, "cs_gamerules") != 0) continue;
    void* rules = nullptr;
    std::memcpy(&rules, static_cast<unsigned char*>(e) + rulesPtr, sizeof rules);
    *proxyOut = e;
    return rules;
  }
  return nullptr;
}

// A planted bomb at `pos` on `site`, ticking with mp_c4timer, defusable as usual.
bool Plant(const float* pos, int site) {
  if (!g_api->engine_feature_available(g_api->self, "entity_create")) return false;
  auto F = [](const char* c, const char* f) { return g_api->schema_offset(g_api->self, c, f); };
  const int ticking = F("CPlantedC4", "m_bBombTicking"), bombSite = F("CPlantedC4", "m_nBombSite"),
            exploded = F("CPlantedC4", "m_bHasExploded"), noDefuse = F("CPlantedC4", "m_bCannotBeDefused");
  if (ticking <= 0 || bombSite <= 0 || exploded <= 0 || noDefuse <= 0) return false;
  void* c4 = g_api->entity_create(g_api->self, "planted_c4");
  if (!c4) return false;
  Put<bool>(c4, ticking, true);
  Put<int32_t>(c4, bombSite, site);
  Put<bool>(c4, exploded, false);
  Put<bool>(c4, noDefuse, false);
  g_api->entity_set_abs_origin(g_api->self, c4, pos);
  if (g_api->entity_spawn(g_api->self, c4) != 1) return false;
  void* proxy = nullptr;
  if (void* rules = GameRules(&proxy)) {
    const int planted = F("CCSGameRules", "m_bBombPlanted"), defused = F("CCSGameRules", "m_bBombDefused");
    if (planted > 0) Put<bool>(rules, planted, true);
    if (defused > 0) Put<bool>(rules, defused, false);
    g_api->entity_mark_changed(g_api->self, proxy);
  }
  return true;
}

// ---- HUD and .guns ------------------------------------------------------------------------------

constexpr const char* kMenuOwner = "retakes";
constexpr const char* kKitLayout = "panorama/layout/custom_game/at_kit.xml";

const ru_hud_v1* Hud() {
  const auto* h = static_cast<const ru_hud_v1*>(g_api->get_interface(g_api->self, RU_HUD_IFACE_NAME, 1));
  if (!h || !h->available || !h->available()) return nullptr;
  return h;
}
const ru_hud_v1* MenuHud() {
  const ru_hud_v1* h = Hud();
  return h && h->struct_size >= offsetof(ru_hud_v1, menu) + sizeof(h->menu) && h->menu ? h : nullptr;
}
std::string Lang(int slot) { return readyup::i18n::LanguageOf(g_api, slot); }
std::string Tr(int slot, const char* key, const Args& args = {}) {
  std::string out;
  for (char ch : g_text.Tr(Lang(slot), key, args)) out += ch == '\n' || ch == '|' ? ' ' : ch;
  return out;
}

retakes::Loadout LoadoutOf(uint64_t id) {
  const auto it = g_guns.find(id);
  return it == g_guns.end() ? retakes::Loadout{} : it->second;
}
std::string GunName(const std::string& cls) {
  for (const auto& gun : retakes::Guns())
    if (cls == gun.cls) return gun.name;
  return cls;
}
void SaveGuns() {
  std::ofstream f(g_gunsFile, std::ios::trunc);
  f << retakes::FormatLoadouts(g_guns);
  if (!f) Log(RU_LOG_WARN, "retakes: could not write %s", g_gunsFile.c_str());
}

// The .guns menu: both sides' rifles (players change sides all the time) and the AWP draw. Each
// button carries CS2's own weapon icon (at_ui.css wpn-*; an addon without them shows the label).
void OnMenuClick(void*, int slot, const char* command);
bool ShowGuns(int slot, uint64_t id) {
  const ru_hud_v1* h = MenuHud();
  if (!h) return false;
  const retakes::Loadout l = LoadoutOf(id);
  std::string spec = "id=retakes-guns\ncolor=acc\nkind=" + Tr(slot, "guns.kind") + "\ntitle=" + Tr(slot, "guns.title") +
                     "\nsub=" + Tr(slot, "guns.sub") + "\n";
  int b = 0;
  for (const auto& gun : retakes::Guns()) {
    const bool on = gun.side == retakes::kT ? l.t == gun.cls : gun.side == retakes::kCT ? l.ct == gun.cls : l.awp;
    const char* tone = gun.side == retakes::kT ? "t" : gun.side == retakes::kCT ? "ct" : "ghost";
    const std::string word = gun.words, first = word.substr(0, word.find(' '));
    spec += "btn=" + std::string(gun.name) + "|" + tone + "|" + (on ? "picked" : "") + "|#g " + first + "|" +
            (gun.side == retakes::kT ? "T" : gun.side == retakes::kCT ? "CT" : Tr(slot, "guns.awp_sub")) + "\n";
    std::string icon = gun.cls + 7;  // weapon_m4a1_silencer -> wpn-m4a1-silencer
    std::replace(icon.begin(), icon.end(), '_', '-');
    spec += "cls.b" + std::to_string(b++) + "=wpn wpn-" + icon + "\n";
  }
  spec += "cols=3\nbw=l\nhint=" + Tr(slot, "guns.hint") + "\nclose=" + Tr(slot, "guns.close") + "|#close\nmouse=on\n";
  h->menu(slot, kMenuOwner, spec.c_str(), &OnMenuClick, nullptr);
  g_gunsOpen.insert(slot);
  return true;
}
void CloseMenu(int slot) {
  g_gunsOpen.erase(slot);
  if (const ru_hud_v1* h = MenuHud()) h->menu(slot, kMenuOwner, "", nullptr, nullptr);
}

void SetGun(int slot, uint64_t id, const std::string& word) {
  retakes::Loadout l = LoadoutOf(id);
  if (!retakes::PickGun(word, &l)) return ChatSlotKey(slot, "guns.usage");
  g_guns[id] = l;
  SaveGuns();
  if (g_gunsOpen.count(slot)) ShowGuns(slot, id);
  else ChatSlotKey(slot, l.awp ? "guns.saved_awp" : "guns.saved", {{"t", GunName(l.t)}, {"ct", GunName(l.ct)}});
}

void OnMenuClick(void*, int slot, const char* command) {
  const std::string c = command ? command : "";
  ru_player p{};
  if (c == "#close" || !g_api->get_player(g_api->self, slot, &p) || !p.steamid64) return CloseMenu(slot);
  if (c.rfind("#g ", 0) == 0) SetGun(slot, p.steamid64, c.substr(3));
}

void OnGuns(void*, const ru_command_ctx* c) {
  if (c->slot < 0 || !c->steamid64) return;
  if (c->argc >= 2) return SetGun(c->slot, c->steamid64, c->argv[1]);
  if (!ShowGuns(c->slot, c->steamid64)) ChatSlotKey(c->slot, "guns.usage");
}

// Freeze time, centre: where to go, in the side's colour, and the players on each side.
void ShowRound(int slot, bool ct, int cts, int ts) {
  const ru_hud_v1* h = MenuHud();
  if (!h || g_gunsOpen.count(slot)) return;
  const std::string site = g.site == 0 ? "A" : "B";
  const std::string spec = "id=retakes-round-" + std::to_string(g.round) + "\ncolor=" + (ct ? "acc" : "warn") +
                           "\nkind=" + Tr(slot, "round.kind", {{"round", std::to_string(g.round)}}) + "\ntitle=" +
                           Tr(slot, ct ? "round.retake_title" : "round.hold_title", {{"site", site}}) +
                           "\nteams=CT|" + std::to_string(std::min(cts, 5)) + "|5|T|" + std::to_string(std::min(ts, 5)) +
                           "|5\nenter=flip\n";
  h->menu(slot, kMenuOwner, spec.c_str(), &OnMenuClick, nullptr);
}

// The queue card (at_kit #at_card, rules body): stays up while the player waits.
void ShowQueue(int slot, int place, int waiting, int playing) {
  const ru_hud_v1* h = Hud();
  const int k = h ? h->open(kKitLayout) : 0;
  if (k <= 0) return;
  auto txt = [&](const std::string& id, const std::string& v) { h->set_text(k, slot, id.c_str(), "text", v.c_str()); };
  auto cls = [&](const std::string& id, const char* c, bool on) { h->set_class(k, slot, id.c_str(), c, on ? 1 : 0); };
  txt("at_kind_a", Tr(slot, "queue.kind"));
  txt("at_text_a", Tr(slot, "queue.title"));
  for (const char* id : {"at_kind_a", "at_text_a"}) cls(id, "cur", true), cls(id, "old", false);
  for (const char* id : {"at_kind_b", "at_text_b"}) cls(id, "cur", false), cls(id, "old", true);
  for (const char* c : {"c-live", "c-warn", "c-ban", "c-muted", "c-acc"}) cls("at_card", c, false);
  for (const char* b : {"b-line", "b-dmg", "b-bar", "chat", "nudge"}) cls("at_card", b, false);
  cls("at_card", "c-info", true);
  cls("at_card", "b-rules", true);
  const std::pair<const char*, std::string> rows[] = {
      {"queue.playing", std::to_string(playing) + " / " + std::to_string(g_set.maxPlayers)},
      {"queue.waiting_count", std::to_string(waiting)},
      {"queue.place", std::to_string(place)}};
  for (int i = 0; i < 3; ++i) {
    txt("at_r" + std::to_string(i) + "_k", Tr(slot, rows[i].first));
    txt("at_r" + std::to_string(i) + "_v", rows[i].second);
  }
  cls("at_card", "on", true);
  g_queueCards.insert(slot);
}
void HideQueue(int slot) {
  if (!g_queueCards.erase(slot)) return;
  const ru_hud_v1* h = Hud();
  if (const int k = h ? h->open(kKitLayout) : 0; k > 0) h->set_class(k, slot, "at_card", "on", 0);
}

// ---- the mode ----------------------------------------------------------------------------------

bool Running() { return g_set.enabled && g.spawnsOk && g.on; }

void Start() {
  const ru_match_v1* m = Match();
  if (m && m->set_external_mode && m->set_external_mode("retakes") != 1) {
    Log(RU_LOG_INFO, "retakes: waiting (a match or practice is on)");
    return;
  }
  g.on = true;
  for (const char* c : kCvars) g_api->server_command(g_api->self, c);
  g_api->server_command(g_api->self, "bot_kick");
  Log(RU_LOG_INFO, "retakes: on %s", g.map.c_str());
}

void OnMapStart(void*, const ru_event*) {
  g = State{};
  const char* cm = g_api->current_map(g_api->self);
  g.map = cm ? cm : "";
  if (!g_set.enabled) return;
  std::string why;
  g.spawnsOk = LoadSpawns(g.map, &why);
  if (!g.spawnsOk) {
    Log(RU_LOG_WARN, "retakes: %s: %s", g.map.c_str(), why.c_str());
    ChatAllKey("map.no_spawns", {{"map", g.map}});
    return;
  }
  Start();
}

void OnRoundPrestart(void*, const char*, const ru_game_event*) {
  if (!Running()) return;
  // Every player on a team plays; spectators who want in wait in order.
  std::vector<retakes::Player> list;
  const auto players = Players();
  for (const auto& p : players) {
    retakes::Player r;
    r.id = IdOf(p);
    r.team = p.team == RU_TEAM_T ? retakes::kT : p.team == RU_TEAM_CT ? retakes::kCT : retakes::kSpectator;
    r.score = g.score.count(r.id) ? g.score[r.id] : 0;
    if (r.team == retakes::kSpectator) {
      if (!g.queued.count(r.id)) g.queued[r.id] = ++g.queueClock;
      r.queued = true;
      r.order = g.queued[r.id];
    }
    list.push_back(r);
  }
  const retakes::Teams next = retakes::NextTeams(list, g.lastWinner, g_set.maxPlayers, g_set.scrambleAfter,
                                                 &g.streak, kShuffle);
  auto teamOf = [&](uint64_t id) {
    if (std::find(next.t.begin(), next.t.end(), id) != next.t.end()) return RU_TEAM_T;
    if (std::find(next.ct.begin(), next.ct.end(), id) != next.ct.end()) return RU_TEAM_CT;
    return RU_TEAM_SPECTATOR;
  };
  const int playing = static_cast<int>(next.t.size() + next.ct.size());
  for (const auto& p : players) {
    const int want = teamOf(IdOf(p));
    if (want != RU_TEAM_SPECTATOR) g.queued.erase(IdOf(p));
    if (p.team != want) g_api->player_change_team(g_api->self, p.slot, want);
    const auto pos = std::find(next.queued.begin(), next.queued.end(), IdOf(p));
    if (want != RU_TEAM_SPECTATOR || pos == next.queued.end()) {
      HideQueue(p.slot);
      continue;
    }
    const int place = static_cast<int>(pos - next.queued.begin()) + 1;
    if (Hud()) ShowQueue(p.slot, place, static_cast<int>(next.queued.size()), playing);
    else ChatSlotKey(p.slot, "queue.waiting", {{"place", std::to_string(place)}});
  }
  if (next.scrambled) ChatAllKey("teams.scrambled");
  g.score.clear();
}

void OnRoundStart(void*, const char*, const ru_game_event*) {
  if (!Running()) return;
  g.site = g.forcedSite >= 0 ? g.forcedSite : static_cast<int>(g_rng() % 2);
  const retakes::SiteSpawns& site = g.spawns.site[g.site];
  std::vector<uint64_t> t, ct;
  std::map<uint64_t, int> slotOf;
  for (const auto& p : Players()) {
    slotOf[IdOf(p)] = p.slot;
    if (p.team == RU_TEAM_T) t.push_back(IdOf(p));
    if (p.team == RU_TEAM_CT) ct.push_back(IdOf(p));
  }
  const retakes::RoundSpawns placed = retakes::PlaceRound(site, t, ct, Plantable(site), kShuffle);
  static const float kStill[3] = {0, 0, 0};
  auto put = [&](const retakes::Placement& pl, const std::vector<retakes::Spawn>& list) {
    if (pl.spawn < 0 || !slotOf.count(pl.id)) return;
    const retakes::Spawn& s = list[static_cast<size_t>(pl.spawn)];
    g_api->player_teleport(g_api->self, slotOf[pl.id], s.pos, s.ang, kStill);
  };
  for (const auto& pl : placed.t) put(pl, site.t);
  for (const auto& pl : placed.ct) put(pl, site.ct);
  g.plantPending = false;
  for (const auto& pl : placed.t) {
    if (pl.id != placed.planter || pl.spawn < 0) continue;
    std::memcpy(g.plantPos, site.t[static_cast<size_t>(pl.spawn)].pos, sizeof g.plantPos);
    g.plantPending = true;
  }
  ++g.round;
  const std::string letter = g.site == 0 ? "A" : "B";
  const bool hud = MenuHud() != nullptr;
  const int cts = static_cast<int>(ct.size()), ts = static_cast<int>(t.size());
  for (const auto& p : Players()) {
    if (p.team != RU_TEAM_T && p.team != RU_TEAM_CT) continue;
    const bool ct = p.team == RU_TEAM_CT;
    if (hud) ShowRound(p.slot, ct, cts, ts);
    else ChatSlotKey(p.slot, ct ? "round.retake" : "round.hold", {{"site", letter}});
  }
}

// Everyone's rifle; one AWP per team, drawn from the players who want it.
void OnPostStart(void*, const char*, const ru_game_event*) {
  if (!Running() || !RU_API_HAS(g_api, player_give_item) || !g_api->player_give_item) return;
  const auto players = Players();
  std::vector<int> want[2];  // T, CT: slots in the AWP draw
  for (const auto& p : players)
    if ((p.team == RU_TEAM_T || p.team == RU_TEAM_CT) && p.steamid64 && LoadoutOf(p.steamid64).awp)
      want[p.team == RU_TEAM_CT].push_back(p.slot);
  int awper[2] = {-1, -1};
  for (int s = 0; s < 2; ++s)
    if (!want[s].empty()) awper[s] = want[s][g_rng() % want[s].size()];
  for (const auto& p : players) {
    if (p.team != RU_TEAM_T && p.team != RU_TEAM_CT) continue;
    const bool ct = p.team == RU_TEAM_CT;
    const retakes::Loadout l = LoadoutOf(p.steamid64);
    g_api->player_give_item(g_api->self, p.slot, p.slot == awper[ct] ? "weapon_awp" : ct ? l.ct.c_str() : l.t.c_str());
  }
}

void OnFreezeEnd(void*, const char*, const ru_game_event*) {
  if (!Running() || !g.plantPending) return;
  g.plantPending = false;
  if (!Plant(g.plantPos, g.site)) Log(RU_LOG_WARN, "retakes: the bomb could not be planted (CS2 build?)");
  // The round panel goes; CS2's own bomb timer takes over.
  if (const ru_hud_v1* h = MenuHud())
    for (const auto& p : Players())
      if (!g_gunsOpen.count(p.slot)) h->menu(p.slot, kMenuOwner, "", nullptr, nullptr);
}

void OnDisconnect(void*, const ru_event* ev) {
  if (!ev || ev->slot < 0) return;
  g_gunsOpen.erase(ev->slot);
  HideQueue(ev->slot);
  if (const ru_hud_v1* h = MenuHud()) h->menu(ev->slot, kMenuOwner, "", nullptr, nullptr);
}

void OnRoundEnd(void*, const char*, const ru_game_event* ev) {
  if (!Running()) return;
  const int w = g_api->ev_get_int(g_api->self, ev, "winner", 0);
  g.lastWinner = w == RU_TEAM_T ? retakes::kT : w == RU_TEAM_CT ? retakes::kCT : 0;
}

void Credit(const ru_game_event* ev, const char* key, int points) {
  const int slot = g_api->ev_get_player_slot(g_api->self, ev, key);
  if (slot < 0) return;
  for (const auto& p : Players())
    if (p.slot == slot) g.score[IdOf(p)] += points;
}
void OnDeath(void*, const char*, const ru_game_event* ev) {
  if (!Running()) return;
  Credit(ev, "attacker", 50);
  Credit(ev, "assister", 25);
}
void OnDefused(void*, const char*, const ru_game_event* ev) {
  if (Running()) Credit(ev, "userid", 50);
}

void Reply(const ru_command_ctx* c, const std::string& msg) {
  if (c->is_console) Log(RU_LOG_INFO, "%s", msg.c_str());
  else if (c->slot >= 0) g_api->chat_to_slot(g_api->self, c->slot, (" \x04[ReadyUp]\x01 " + msg).c_str());
}

void OnRu(void*, const ru_command_ctx* c) {
  std::string reply;
  if (c->argc >= 4 && std::strcmp(c->argv[2], "site") == 0) {
    const std::string s = c->argv[3];
    g.forcedSite = s == "a" || s == "A" ? 0 : s == "b" || s == "B" ? 1 : -1;
    reply = "retakes: site " + std::string(g.forcedSite < 0 ? "random" : g.forcedSite == 0 ? "A" : "B");
  } else {
    char buf[256];
    std::snprintf(buf, sizeof buf, "retakes %s: %s on %s, max %d players, T streak %d", RETAKES_VERSION,
                  Running() ? "running" : g.spawnsOk ? "waiting" : "no retake spawns", g.map.c_str(),
                  g_set.maxPlayers, g.streak.tWins);
    reply = buf;
  }
  Reply(c, reply);
}

void LoadConfig() {
  char buf[64];
  auto num = [&](const char* key, int lo, int hi, int* out) {
    if (g_api->config_get(g_api->self, key, buf, sizeof buf) <= 0) return;
    const int v = std::atoi(buf);
    if (v >= lo && v <= hi) *out = v;
  };
  if (g_api->config_get(g_api->self, "enabled", buf, sizeof buf) > 0) g_set.enabled = buf[0] != '0';
  num("max_players", 2, 10, &g_set.maxPlayers);
  num("scramble_after", 0, 30, &g_set.scrambleAfter);
}

}  // namespace

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info), (1u << 16) | 13u, "retakes", RETAKES_VERSION, "Ready Up",
      "retakes: the bomb is planted, Ts hold the site, CTs retake it (CS2's own retake spawns)",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, entity_spawn)) return 1;
  g_api = api;
  g_text = {};
  if (const std::string err = g_text.Load(kReadyUpLocales, kReadyUpLocaleCount); !err.empty())
    ru_logf(api, RU_LOG_WARN, "locales: %s", err.c_str());
  const char* dir = api->data_dir(api->self);  // csgo/readyup/plugins/retakes
  g_csgoDir = dir ? dir : "";
  while (!g_csgoDir.empty() && g_csgoDir.back() == '/') g_csgoDir.pop_back();
  for (int up = 0; up < 3 && g_csgoDir.rfind('/') != std::string::npos; ++up) g_csgoDir.erase(g_csgoDir.rfind('/'));
  LoadConfig();
  g_gunsFile = std::string(dir ? dir : ".") + "/guns.txt";
  {
    std::ifstream f(g_gunsFile);
    std::stringstream text;
    text << f.rdbuf();
    g_guns = retakes::ParseLoadouts(text.str());
  }
  api->register_ru_subcommand(api->self, "retakes", &OnRu, nullptr);
  // Literal names: compat-report checks each against the CS2 build.
  auto sub = [&](int ok, const char* name) {
    if (!ok) ru_logf(api, RU_LOG_WARN, "could not subscribe to %s", name);
  };
  sub(api->subscribe_game_event(api->self, "round_prestart", &OnRoundPrestart, nullptr) != 0, "round_prestart");
  sub(api->subscribe_game_event(api->self, "round_start", &OnRoundStart, nullptr) != 0, "round_start");
  sub(api->subscribe_game_event(api->self, "round_poststart", &OnPostStart, nullptr) != 0, "round_poststart");
  sub(api->subscribe_game_event(api->self, "round_freeze_end", &OnFreezeEnd, nullptr) != 0, "round_freeze_end");
  sub(api->subscribe_game_event(api->self, "round_end", &OnRoundEnd, nullptr) != 0, "round_end");
  sub(api->subscribe_game_event(api->self, "player_death", &OnDeath, nullptr) != 0, "player_death");
  sub(api->subscribe_game_event(api->self, "bomb_defused", &OnDefused, nullptr) != 0, "bomb_defused");
  api->subscribe(api->self, RU_EVENT_MAP_START, &OnMapStart, nullptr);
  api->subscribe(api->self, RU_EVENT_PLAYER_DISCONNECT, &OnDisconnect, nullptr);
  if (!api->register_chat_command(api->self, ".guns", &OnGuns, nullptr))
    ru_logf(api, RU_LOG_WARN, "could not register `.guns` (another plugin owns it)");
  // Loaded on a running map (a plugins.set): start now.
  OnMapStart(nullptr, nullptr);
  ru_logf(api, RU_LOG_INFO, "loaded " RETAKES_VERSION);
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  if (const ru_hud_v1* h = MenuHud()) h->menu(RU_HUD_ALL, kMenuOwner, "", nullptr, nullptr);
  for (int slot : std::set<int>(g_queueCards)) HideQueue(slot);
  if (g.on)
    if (const ru_match_v1* m = Match(); m && m->set_external_mode) m->set_external_mode("");
  g_api = nullptr;
}

}  // extern "C"
