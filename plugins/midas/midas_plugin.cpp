// readyup-midas: a fun plugin. Every weapon a player in `midas_steamids` picks up (buys, gets at
// spawn, picks off the floor) turns gold: the weapon entity's m_clrRender is set to `color`
// (default 255,200,40). The server can't send clients custom textures, so a render colour is what
// a plugin can do; it stays on the weapon when someone else picks it up (Midas touch).
//
//   cfg/ReadyUp/midas.cfg (or readyup.cfg [midas]), re-read every 5 s:
//     enabled=0              off by default
//     midas_steamids=7656119...,7656119...
//     color=255,200,40       r,g,b[,a]
//
// Never active under the valve ruleset (the match plugin's ruleset, else readyup.cfg's): going
// inert puts every weapon it tinted back to white, and so does unloading. `ru plugin reload
// midas` therefore restores, then the new image tints again on the next pickup / spawn.
// Engine access through ru_api only (schema offsets, entity handles, entity_mark_changed).
#include "midas_rules.h"

#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

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
std::set<uint64_t> g_midas;
Rgba g_color = kGold;
std::string g_ruleset = "default";
bool g_active = false;
double g_lastConfig = -1e9;
int g_pending[65] = {};             // ticks left to (re)check a slot's weapons after an event
std::set<uint32_t> g_tinted;        // weapon handles this image tinted (restored when inert / unloading)

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

void RestoreAll(const char* why) {
  if (g_tinted.empty()) return;
  int n = 0;
  if (g_off.Ok() && g_api->entity_system_status(g_api->self) == RU_ENTSYS_OK) {
    for (uint32_t h : g_tinted) {
      if (void* w = g_api->entity_from_handle(g_api->self, h)) n += SetColor(w, kWhite) ? 1 : 0;
    }
  }
  ru_logf(g_api, RU_LOG_INFO, "restored %d weapon(s) to their normal colour (%s)", n, why);
  g_tinted.clear();
}

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
      RestoreAll(g_enabled ? "valve ruleset" : "disabled");
      ru_logf(g_api, RU_LOG_INFO, "inert (%s)", g_enabled ? "valve ruleset" : "enabled=0");
    }
  }
}

// Tints the weapons a Midas player holds now.
void TintSlot(int slot) {
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ctrl) return;
  const uint64_t sid = Rd<uint64_t>(ctrl, g_off.ctrl_steamId);
  if (!ShouldTint(g_active, g_midas, sid)) return;
  void* pawn = g_api->entity_from_handle(g_api->self, Rd<uint32_t>(ctrl, g_off.ctrl_playerPawn));
  if (!pawn) return;
  void* ws = Rd<void*>(pawn, g_off.pawn_weaponServices);
  if (!ws) return;
  // CNetworkUtlVectorBase<CHandle<CBasePlayerWeapon>>: { int m_Size; <pad>; CHandle* m_pElements; }
  const int count = Rd<int32_t>(ws, g_off.ws_myWeapons);
  const uint32_t* handles = Rd<const uint32_t*>(ws, g_off.ws_myWeapons + 8);
  if (!handles || count <= 0 || count > 64) return;
  for (int i = 0; i < count; ++i) {
    void* w = g_api->entity_from_handle(g_api->self, handles[i]);
    if (!w) continue;
    if (SetColor(w, g_color)) {
      g_tinted.insert(handles[i]);
      if (g_api->debug_enabled(g_api->self)) {
        const char* cn = g_api->entity_classname(g_api->self, w);
        ru_logf(g_api, RU_LOG_DEBUG, "tinted %s of slot %d", cn ? cn : "?", slot);
      }
    }
  }
}

void OnTick(void*, const ru_tick_info* t) {
  RefreshConfig(t->now);
  ResolveOffsets();
  if (!g_active || !g_off.Ok() || g_midas.empty()) return;
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

void OnMap(void*, const ru_event*) {
  // New map: new entities; old handles mean nothing.
  g_tinted.clear();
  g_off.resolved = false;
}

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  char line[160];
  if (!g_enabled) {
    add(ctx, "INFO", "midas", "off (enabled=0)");
  } else if (!g_active) {
    add(ctx, "INFO", "midas", "inert (valve ruleset)");
  } else {
    std::snprintf(line, sizeof(line), "active: %zu player(s), %zu weapon(s) tinted", g_midas.size(), g_tinted.size());
    add(ctx, "INFO", "midas", line);
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
      "fun: weapons picked up by the players in midas_steamids turn gold (off by default)",
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
  g_color = kGold;
  g_lastConfig = -1e9;
  for (int& p : g_pending) p = 0;
  api->on_tick(api->self, OnTick, nullptr);
  api->subscribe(api->self, RU_EVENT_MAP_START, OnMap, nullptr);
  for (const char* e : {"item_pickup", "item_equip", "player_spawn"}) {
    if (!api->subscribe_game_event(api->self, e, OnItemEvent, nullptr)) ru_logf(api, RU_LOG_WARN, "could not subscribe to %s", e);
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
