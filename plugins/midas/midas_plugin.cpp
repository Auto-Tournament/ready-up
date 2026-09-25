// readyup-midas: a fun plugin. Every weapon a Midas player picks up (buys, gets at spawn, picks
// off the floor) turns gold. It stays gold when someone else picks it up (Midas touch).
//
// Gold, two ways (`finish`):
//   - a gold paint kit (`paint_kit`, default 1025 "Gold Brick") through the skins plugin's
//     readyup.skins.v1 paint_weapon, when skins.so is loaded and active (finish=auto, default);
//   - else, and for knives / grenades / C4, the render colour: m_clrRender = `color`
//     (default 255,200,40). The server can't send clients custom textures.
//
// Who is Midas: the players in `midas_steamids`, plus with best_player=1 the best player of the
// map (top ADR or kills, `best_player_stat`) from the match plugin's stats (readyup.match.v1
// map_stats), picked a few ticks after a round start (every round once
// `best_player_min_rounds` are played, or at each new half: `best_player_when`). Scrims only
// unless best_player_in_matches=1.
//
//   cfg/ReadyUp/midas.cfg (or readyup.cfg [midas]), re-read every 5 s: see that file.
//
// Never active under the valve ruleset (the match plugin's ruleset, else readyup.cfg's): going
// inert puts every weapon it touched back (white / handed back to skins.so), and so does
// unloading. `ru plugin reload midas` therefore restores, then the new image paints again on the
// next pickup / spawn. Engine access through ru_api only (schema offsets, entity handles,
// entity_mark_changed); paint kits only through skins.so.
#include "midas_rules.h"

#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"
#include "readyup/skins_iface.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#ifndef MIDAS_VERSION
#define MIDAS_VERSION "dev"
#endif

namespace midas {
namespace {

const ru_api* g_api = nullptr;

struct Offsets {
  bool resolved = false;
  int ctrl_playerPawn = -1;      // CCSPlayerController::m_hPlayerPawn (CHandle)
  int ctrl_steamId = -1;         // CBasePlayerController::m_steamID
  int pawn_weaponServices = -1;  // CBasePlayerPawn::m_pWeaponServices (ptr)
  int ws_myWeapons = -1;         // CPlayer_WeaponServices::m_hMyWeapons (CNetworkUtlVectorBase<CHandle>)
  int clrRender = -1;            // CBaseModelEntity::m_clrRender (Color: r, g, b, a bytes)
  bool Ok() const {
    return ctrl_playerPawn >= 0 && ctrl_steamId >= 0 && pawn_weaponServices >= 0 && ws_myWeapons >= 0 && clrRender >= 0;
  }
} g_off;

// Config (refreshed every 5 s) and state.
bool g_enabled = false;
std::set<uint64_t> g_midas;         // midas_steamids
Rgba g_color = kGold;
Finish g_finish = Finish::kAuto;
int g_paintKit = kGoldPaintKit;
float g_paintWear = 0.0f;
int g_paintSeed = 0;
bool g_bestOn = false;              // best_player
bool g_bestInMatches = false;       // best_player_in_matches
BestStat g_bestStat = BestStat::kAdr;
BestWhen g_bestWhen = BestWhen::kRound;
int g_bestMinRounds = 3;
std::string g_ruleset = "default";
bool g_active = false;
double g_lastConfig = -1e9;
int g_pending[65] = {};             // ticks left to (re)check a slot's weapons after an event
std::set<uint32_t> g_tinted;        // weapon handles this image tinted (restored when inert / unloading)
std::set<uint32_t> g_painted;       // weapon handles painted through skins.so (handed back likewise)
uint64_t g_best = 0;                // the best player's SteamID64 while they are Midas, else 0
std::string g_bestLine;             // why (selftest)
int g_lastPickHalf = 0;             // best_player_when=half: the half the last pick was for
int g_pickIn = 0;                   // ticks until the pick after a round start (0 = none pending)

std::string ConfigValue(const char* key) {
  char buf[512] = {};
  return g_api->config_get(g_api->self, key, buf, sizeof(buf)) > 0 ? std::string(buf) : std::string();
}

void ResolveOffsets() {
  if (g_off.resolved) return;
  if (g_api->entity_system_status(g_api->self) != RU_ENTSYS_OK) return;
  auto F = [](const char* c, const char* f) { return g_api->schema_offset(g_api->self, c, f); };
  g_off.ctrl_playerPawn = F("CCSPlayerController", "m_hPlayerPawn");
  g_off.ctrl_steamId = F("CBasePlayerController", "m_steamID");
  g_off.pawn_weaponServices = F("CBasePlayerPawn", "m_pWeaponServices");
  g_off.ws_myWeapons = F("CPlayer_WeaponServices", "m_hMyWeapons");
  g_off.clrRender = F("CBaseModelEntity", "m_clrRender");
  g_off.resolved = true;
  if (!g_off.Ok()) {
    ru_logf(g_api, RU_LOG_WARN, "schema fields missing (m_clrRender=%d, weapons=%d/%d); no tint", g_off.clrRender,
            g_off.pawn_weaponServices, g_off.ws_myWeapons);
  }
}

template <typename T>
T Rd(const void* base, int off) {
  T v{};
  std::memcpy(&v, static_cast<const unsigned char*>(base) + off, sizeof(T));
  return v;
}

// Writes the colour when it differs. True if written.
bool SetColor(void* ent, const Rgba& c) {
  unsigned char* p = static_cast<unsigned char*>(ent) + g_off.clrRender;
  const unsigned char want[4] = {c.r, c.g, c.b, c.a};
  if (std::memcmp(p, want, 4) == 0) return false;
  std::memcpy(p, want, 4);
  g_api->entity_mark_changed(g_api->self, ent);
  return true;
}

// skins.so's readyup.skins.v1 with paint_weapon, or NULL (not loaded / reloading). Look it up in
// every callback: the pointer is only valid until skins.so can unload.
const ru_skins_v1* Skins() {
  const auto* s = static_cast<const ru_skins_v1*>(g_api->get_interface(g_api->self, RU_SKINS_IFACE_NAME, 1));
  return s && RU_API_HAS(s, paint_weapon) && s->active && s->paint_weapon ? s : nullptr;
}

bool IsMidas(uint64_t sid) { return ShouldTint(g_active, g_midas, sid) || (g_active && sid != 0 && sid == g_best); }

// Puts one weapon back: white, and handed back to skins.so if it painted it. True if it was ours.
bool RestoreWeapon(uint32_t h, const ru_skins_v1* skins) {
  bool ours = false;
  void* w = g_api->entity_from_handle(g_api->self, h);
  if (g_tinted.erase(h)) {
    ours = true;
    if (w) SetColor(w, kWhite);
  }
  if (g_painted.erase(h)) {
    ours = true;
    if (w && skins) skins->paint_weapon(h, 0, 0, 0.0f, 0);
  }
  return ours;
}

void RestoreAll(const char* why) {
  if (g_tinted.empty() && g_painted.empty()) return;
  int n = 0;
  if (g_off.Ok() && g_api->entity_system_status(g_api->self) == RU_ENTSYS_OK) {
    const ru_skins_v1* skins = Skins();
    std::set<uint32_t> all = g_tinted;
    all.insert(g_painted.begin(), g_painted.end());
    for (uint32_t h : all) n += RestoreWeapon(h, skins) ? 1 : 0;
  }
  ru_logf(g_api, RU_LOG_INFO, "restored %d weapon(s) to their normal look (%s)", n, why);
  g_tinted.clear();
  g_painted.clear();
}

// Calls fn(handle, weapon) for each weapon the player in `slot` holds; returns their SteamID64
// (0: no controller / pawn).
template <typename Fn>
uint64_t ForHeldWeapons(int slot, Fn&& fn) {
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ctrl) return 0;
  const uint64_t sid = Rd<uint64_t>(ctrl, g_off.ctrl_steamId);
  void* pawn = g_api->entity_from_handle(g_api->self, Rd<uint32_t>(ctrl, g_off.ctrl_playerPawn));
  if (!pawn) return sid;
  void* ws = Rd<void*>(pawn, g_off.pawn_weaponServices);
  if (!ws) return sid;
  // CNetworkUtlVectorBase<CHandle<CBasePlayerWeapon>>: { int m_Size; <pad>; CHandle* m_pElements; }
  const int count = Rd<int32_t>(ws, g_off.ws_myWeapons);
  const uint32_t* handles = Rd<const uint32_t*>(ws, g_off.ws_myWeapons + 8);
  if (!handles || count <= 0 || count > 64) return sid;
  for (int i = 0; i < count; ++i) {
    if (void* w = g_api->entity_from_handle(g_api->self, handles[i])) fn(handles[i], w);
  }
  return sid;
}

void SetBest(uint64_t sid, const std::string& why);
void CheckBestAllowed();

void RefreshConfig(double now) {
  if (now - g_lastConfig < 5.0) return;
  g_lastConfig = now;
  g_enabled = ParseBool(ConfigValue("enabled"), false);
  int bad = 0;
  const std::string ids = ConfigValue("midas_steamids");
  auto list = ParseSteamIds(ids, &bad);
  if (list != g_midas) {
    ru_logf(g_api, RU_LOG_INFO, "midas_steamids: %zu player(s)%s", list.size(), bad ? " (some entries are not SteamID64s)" : "");
    g_midas = std::move(list);
    for (int& p : g_pending) p = 16;  // check everyone's weapons again
  }
  Rgba c = kGold;
  const std::string cs = ConfigValue("color");
  if (!cs.empty() && !ParseColor(cs, &c)) ru_logf(g_api, RU_LOG_WARN, "color \"%s\" is not r,g,b[,a]; using gold", cs.c_str());
  if (c != g_color) {
    g_color = c;
    for (int& p : g_pending) p = 16;
  }
  // finish / paint kit: a change puts everything back, then paints again.
  Finish finish = Finish::kAuto;
  const std::string fs = ConfigValue("finish");
  if (!fs.empty() && !ParseFinish(fs, &finish)) ru_logf(g_api, RU_LOG_WARN, "finish \"%s\" is not auto|tint; using auto", fs.c_str());
  const int kit = ParseInt(ConfigValue("paint_kit"), kGoldPaintKit, 1, 100000);
  const float wear = ParseFloat(ConfigValue("paint_wear"), 0.0f, 0.0f, 1.0f);
  const int seed = ParseInt(ConfigValue("paint_seed"), 0, 0, 1000);
  if (finish != g_finish || kit != g_paintKit || wear != g_paintWear || seed != g_paintSeed) {
    g_finish = finish;
    g_paintKit = kit;
    g_paintWear = wear;
    g_paintSeed = seed;
    RestoreAll("finish changed");  // no-op on the first read
    ru_logf(g_api, RU_LOG_INFO, "finish %s, paint kit %d (wear %.2f, seed %d)", finish == Finish::kTint ? "tint" : "auto",
            kit, static_cast<double>(wear), seed);
    for (int& p : g_pending) p = 16;
  }
  // Best player.
  g_bestOn = ParseBool(ConfigValue("best_player"), false);
  g_bestInMatches = ParseBool(ConfigValue("best_player_in_matches"), false);
  BestStat stat = BestStat::kAdr;
  const std::string ss = ConfigValue("best_player_stat");
  if (!ss.empty() && !ParseBestStat(ss, &stat)) ru_logf(g_api, RU_LOG_WARN, "best_player_stat \"%s\" is not adr|kills; using adr", ss.c_str());
  BestWhen when = BestWhen::kRound;
  const std::string ws = ConfigValue("best_player_when");
  if (!ws.empty() && !ParseBestWhen(ws, &when)) ru_logf(g_api, RU_LOG_WARN, "best_player_when \"%s\" is not round|half; using round", ws.c_str());
  g_bestStat = stat;
  g_bestWhen = when;
  g_bestMinRounds = ParseInt(ConfigValue("best_player_min_rounds"), 3, 0, 30);
  std::string ruleset = "default";
  const auto* m = static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
  if (m && RU_API_HAS(m, ruleset) && m->ruleset && m->ruleset()) ruleset = m->ruleset();
  else if (!ConfigValue("ruleset").empty()) ruleset = ConfigValue("ruleset");
  g_ruleset = ruleset;
  const bool active = Active(g_enabled, g_ruleset);
  if (active != g_active) {
    g_active = active;
    if (active) {
      ru_logf(g_api, RU_LOG_INFO, "active: %zu Midas player(s), colour %d,%d,%d,%d", g_midas.size(), g_color.r,
              g_color.g, g_color.b, g_color.a);
      for (int& p : g_pending) p = 16;
    } else {
      g_best = 0;
      g_bestLine.clear();
      RestoreAll(g_enabled ? "valve ruleset" : "disabled");
      ru_logf(g_api, RU_LOG_INFO, "inert (%s)", g_enabled ? "valve ruleset" : "enabled=0");
    }
  }
  CheckBestAllowed();
}

// ---- best player ------------------------------------------------------------------------------

const ru_match_v1* Match() {
  const auto* m = static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
  return m && RU_API_HAS(m, map_stats) && m->map_stats ? m : nullptr;
}

bool MapInfo(const ru_match_v1* m, ru_match_map_info* info, std::vector<PlayerTotals>* players) {
  *info = ru_match_map_info{};
  info->struct_size = sizeof(*info);
  auto collect = [](void* user, const ru_match_player_stats* p) {
    if (!p || p->struct_size < sizeof(ru_match_player_stats)) return;
    PlayerTotals t;
    t.steamid64 = p->steamid64;
    t.kills = p->kills;
    t.deaths = p->deaths;
    t.damage = p->damage;
    t.rounds = p->rounds_played;
    static_cast<std::vector<PlayerTotals>*>(user)->push_back(t);
  };
  return m->map_stats(info, players ? +collect : nullptr, players) == 1;
}

bool AllowedNow(const ru_match_map_info& info) {
  return g_active && BestPlayerAllowed(g_bestOn, g_bestInMatches, info.live != 0, info.scrim != 0, g_ruleset);
}

// Every 5 s: the rule switched off, the map stopped recording (postgame, idle) or a real match
// without best_player_in_matches -> no best-player Midas.
void CheckBestAllowed() {
  if (g_best == 0) return;
  const ru_match_v1* m = Match();
  ru_match_map_info info{};
  if (!m || !MapInfo(m, &info, nullptr) || !AllowedNow(info)) SetBest(0, "");
}

std::string PlayerName(uint64_t sid) {
  ru_player p{};
  p.struct_size = sizeof(p);
  if (g_api->get_player_by_steamid(g_api->self, sid, &p) == 1 && p.name[0]) return p.name;
  return std::to_string(sid);
}

// A new best-player Midas (0 = none). The previous one's held weapons go back to normal (unless
// they are on midas_steamids); weapons they dropped stay gold.
void SetBest(uint64_t sid, const std::string& why) {
  if (sid == g_best) return;
  const uint64_t old = g_best;
  g_best = sid;
  g_bestLine = sid ? PlayerName(sid) + " (" + why + ")" : std::string();
  if (old != 0 && !g_midas.count(old) && g_off.Ok()) {
    const int slot = g_api->slot_for_steamid(g_api->self, old);
    if (slot >= 0 && slot < 64) {
      const ru_skins_v1* skins = Skins();
      ForHeldWeapons(slot, [&](uint32_t h, void*) { RestoreWeapon(h, skins); });
    }
  }
  if (sid == 0) {
    if (old != 0) ru_logf(g_api, RU_LOG_INFO, "best player: no Midas now");
    return;
  }
  const int slot = g_api->slot_for_steamid(g_api->self, sid);
  if (slot >= 0 && slot < 64) g_pending[slot] = 16;
  const std::string name = PlayerName(sid);
  ru_logf(g_api, RU_LOG_INFO, "best player: %s (%llu) is Midas: %s", name.c_str(), static_cast<unsigned long long>(sid),
          why.c_str());
  g_api->chat_all(g_api->self, ("Midas: " + name + " has the golden touch (" + why + ").").c_str(), 0);
}

// A few ticks after a round start (the match plugin handles round_start on its tick first).
void PickBestPlayer() {
  const ru_match_v1* m = Match();
  std::vector<PlayerTotals> all;
  ru_match_map_info info{};
  if (!m || !MapInfo(m, &info, &all) || !AllowedNow(info)) {
    SetBest(0, "");
    return;
  }
  if (!PickNow(g_bestWhen, info.rounds, g_bestMinRounds, info.half, g_lastPickHalf)) return;
  if (g_bestWhen == BestWhen::kHalf) g_lastPickHalf = info.half;
  std::vector<PlayerTotals> connected;
  for (const auto& p : all) {
    if (g_api->slot_for_steamid(g_api->self, p.steamid64) >= 0) connected.push_back(p);
  }
  const uint64_t best = midas::PickBest(connected, g_bestStat, g_best);
  std::string why;
  for (const auto& p : connected) {
    if (p.steamid64 != best) continue;
    char buf[96];
    if (g_bestStat == BestStat::kAdr) std::snprintf(buf, sizeof(buf), "best ADR: %.0f", Adr(p));
    else std::snprintf(buf, sizeof(buf), "most kills: %d", p.kills);
    why = buf;
  }
  SetBest(best, why);
}

// Gilds the weapons a Midas player holds now: the paint kit through skins.so where it can,
// else the tint.
void TintSlot(int slot) {
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ctrl || !IsMidas(Rd<uint64_t>(ctrl, g_off.ctrl_steamId))) return;
  const ru_skins_v1* skins = g_finish == Finish::kAuto ? Skins() : nullptr;
  const bool paint = skins && skins->active() == 1;
  ForHeldWeapons(slot, [&](uint32_t h, void* w) {
    if (g_painted.count(h)) return;
    const char* cn = g_api->entity_classname(g_api->self, w);
    const uint64_t sid = Rd<uint64_t>(ctrl, g_off.ctrl_steamId);
    if (paint && cn && Paintable(cn) && skins->paint_weapon(h, sid, g_paintKit, g_paintWear, g_paintSeed) == 1) {
      g_painted.insert(h);
      if (g_tinted.erase(h)) SetColor(w, kWhite);  // tinted before skins.so was there
      if (g_api->debug_enabled(g_api->self)) ru_logf(g_api, RU_LOG_DEBUG, "painted %s of slot %d", cn, slot);
      return;
    }
    if (SetColor(w, g_color)) {
      g_tinted.insert(h);
      if (g_api->debug_enabled(g_api->self)) ru_logf(g_api, RU_LOG_DEBUG, "tinted %s of slot %d", cn ? cn : "?", slot);
    }
  });
}

void OnTick(void*, const ru_tick_info* t) {
  RefreshConfig(t->now);
  ResolveOffsets();
  if (g_pickIn > 0 && --g_pickIn == 0 && g_active && g_bestOn) PickBestPlayer();
  if (!g_active || !g_off.Ok() || (g_midas.empty() && g_best == 0)) return;
  for (int s = 0; s < 64; ++s) {
    if (g_pending[s] <= 0) continue;
    // Right after the event and a few ticks later (the weapon entity can show up a tick late).
    if (g_pending[s] == 16 || g_pending[s] == 12 || g_pending[s] == 4 || g_pending[s] == 1) TintSlot(s);
    --g_pending[s];
  }
}

// item_pickup / item_equip / player_spawn: check that player's weapons over the next ticks.
void OnItemEvent(void*, const char*, const ru_game_event* ev) {
  const int slot = g_api->ev_get_player_slot(g_api->self, ev, "userid");
  if (slot >= 0 && slot < 64) g_pending[slot] = 16;
}

// round_start: pick the best player a few ticks from now.
void OnRoundStart(void*, const char*, const ru_game_event*) { g_pickIn = 8; }

void OnMap(void*, const ru_event*) {
  // New map: new entities; old handles mean nothing. New stats: no best player yet.
  g_tinted.clear();
  g_painted.clear();
  g_best = 0;
  g_bestLine.clear();
  g_lastPickHalf = 0;
  g_pickIn = 0;
  g_off.resolved = false;
}

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  char line[160];
  if (!g_enabled) {
    add(ctx, "INFO", "midas", "off (enabled=0)");
  } else if (!g_active) {
    add(ctx, "INFO", "midas", "inert (valve ruleset)");
  } else {
    std::snprintf(line, sizeof(line), "active: %zu player(s), %zu weapon(s) tinted, %zu painted (%s)", g_midas.size(),
                  g_tinted.size(), g_painted.size(),
                  g_finish == Finish::kTint ? "finish tint" : Skins() ? "paint kit via skins.so" : "no skins.so: tint");
    add(ctx, "INFO", "midas", line);
    if (g_bestOn) {
      std::snprintf(line, sizeof(line), "best player (%s, per %s): %s", g_bestStat == BestStat::kAdr ? "ADR" : "kills",
                    g_bestWhen == BestWhen::kHalf ? "half" : "round", g_best ? g_bestLine.c_str() : "nobody yet");
      add(ctx, "INFO", "midas best", line);
    }
  }
  if (g_off.resolved) {
    std::snprintf(line, sizeof(line), "CBaseModelEntity::m_clrRender offset %d", g_off.clrRender);
    add(ctx, g_off.clrRender >= 0 ? "OK" : "WARN", "midas schema", line);
  }
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

}  // namespace
}  // namespace midas

using namespace midas;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 1u,  // needs API 1.1 (entities, schema, raw events, config, interfaces)
      "midas",
      MIDAS_VERSION,
      "Ready Up",
      "fun: Midas players' weapons turn gold (midas_steamids, or the best player); off by default",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, get_interface)) return 1;
  g_api = api;
  g_off = Offsets{};
  g_enabled = g_active = false;
  g_midas.clear();
  g_tinted.clear();
  g_painted.clear();
  g_color = kGold;
  g_finish = Finish::kAuto;
  g_paintKit = kGoldPaintKit;
  g_paintWear = 0.0f;
  g_paintSeed = 0;
  g_bestOn = false;
  g_best = 0;
  g_bestLine.clear();
  g_lastPickHalf = 0;
  g_pickIn = 0;
  g_lastConfig = -1e9;
  for (int& p : g_pending) p = 0;
  api->on_tick(api->self, OnTick, nullptr);
  api->subscribe(api->self, RU_EVENT_MAP_START, OnMap, nullptr);
  for (const char* e : {"item_pickup", "item_equip", "player_spawn"}) {
    if (!api->subscribe_game_event(api->self, e, OnItemEvent, nullptr)) ru_logf(api, RU_LOG_WARN, "could not subscribe to %s", e);
  }
  if (!api->subscribe_game_event(api->self, "round_start", OnRoundStart, nullptr)) {
    ru_logf(api, RU_LOG_WARN, "could not subscribe to round_start (no best player)");
  }
  if (RU_API_HAS(api, provide_interface)) {
    api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "midas", RU_SELFTEST_IFACE_VERSION,
                           const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  }
  ru_logf(api, RU_LOG_INFO, "loaded " MIDAS_VERSION " (enable with enabled=1 and midas_steamids in cfg/ReadyUp/midas.cfg)");
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  // Hot reload / unload: weapons go back to their normal colour; the next image tints again.
  RestoreAll("unload");
  ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
