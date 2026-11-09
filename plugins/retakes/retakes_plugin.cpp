// readyup-retakes: retakes for fun servers. Every round the bomb is already planted on one site;
// the Ts hold it, the CTs retake it.
//
// Spawns are CS2's own: maps/<map>_retake.txt, read from the server's pak01_dir.vpk at map start
// (retakes_rules.h). On a map without one the plugin waits (a chat line says so). Rounds:
//   round_prestart  the next teams (queue past max_players, scramble after a T streak, the CTs
//                   who scored attack after a CT win) and the team changes;
//   round_start     a site, everyone on that site's spawns, the planter on the most central T spawn;
//   freeze end      the bomb is planted where the planter stands (a planted_c4 entity).
// Loadouts are CS2's default-loadout cvars (T AK-47, CT M4A1-S, Deagle, armor + helmet, kits).
// While it runs, the match plugin is in external mode "retakes" (readyup.match.v1), so no scrim
// warmup or idle cfg gets in the way. A match load ends it.
//
// Console: `ru retakes` (status), `ru retakes site a|b|random`.
#include "retakes_rules.h"

#include "readyup/i18n.h"
#include "readyup/i18n_api.h"
#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <random>
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
    "mp_t_default_primary weapon_ak47",           "mp_ct_default_primary weapon_m4a1_silencer",
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
} g;

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
  for (const auto& p : players) {
    const int want = teamOf(IdOf(p));
    if (want != RU_TEAM_SPECTATOR) g.queued.erase(IdOf(p));
    if (p.team != want) g_api->player_change_team(g_api->self, p.slot, want);
    if (want == RU_TEAM_SPECTATOR && (p.team == RU_TEAM_T || p.team == RU_TEAM_CT || p.team == RU_TEAM_SPECTATOR)) {
      const auto pos = std::find(next.queued.begin(), next.queued.end(), IdOf(p));
      if (pos != next.queued.end())
        ChatSlotKey(p.slot, "queue.waiting", {{"place", std::to_string(pos - next.queued.begin() + 1)}});
    }
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
  const std::string letter = g.site == 0 ? "A" : "B";
  for (const auto& p : Players()) {
    if (p.team == RU_TEAM_T) ChatSlotKey(p.slot, "round.hold", {{"site", letter}});
    if (p.team == RU_TEAM_CT) ChatSlotKey(p.slot, "round.retake", {{"site", letter}});
  }
}

void OnFreezeEnd(void*, const char*, const ru_game_event*) {
  if (!Running() || !g.plantPending) return;
  g.plantPending = false;
  if (!Plant(g.plantPos, g.site)) Log(RU_LOG_WARN, "retakes: the bomb could not be planted (CS2 build?)");
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
  api->register_ru_subcommand(api->self, "retakes", &OnRu, nullptr);
  const struct {
    const char* name;
    ru_game_event_fn fn;
  } events[] = {{"round_prestart", &OnRoundPrestart}, {"round_start", &OnRoundStart},
                {"round_freeze_end", &OnFreezeEnd},  {"round_end", &OnRoundEnd},
                {"player_death", &OnDeath},          {"bomb_defused", &OnDefused}};
  for (const auto& e : events)
    if (!api->subscribe_game_event(api->self, e.name, e.fn, nullptr))
      ru_logf(api, RU_LOG_WARN, "could not subscribe to %s", e.name);
  api->subscribe(api->self, RU_EVENT_MAP_START, &OnMapStart, nullptr);
  // Loaded on a running map (a plugins.set): start now.
  OnMapStart(nullptr, nullptr);
  ru_logf(api, RU_LOG_INFO, "loaded " RETAKES_VERSION);
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  if (g.on)
    if (const ru_match_v1* m = Match(); m && m->set_external_mode) m->set_external_mode("");
  g_api = nullptr;
}

}  // extern "C"
